// The launcher's native side on Windows: Win32 windows with WebView2 (through webview/webview,
// which loads the WebView2 runtime itself), the interface served to WebView2 from memory, the
// shell's file dialogs, and ShellExecute.

#include <windows.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>

#include "webview.h"

#include "app/assets.hpp"
#include "app/native.hpp"
#include "launch/env.hpp"

namespace sf::app {

const char* const kOrigin = "https://starforge.localhost";
const char* const kCsp =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; connect-src 'none'; "
    "object-src 'none'; frame-src 'none'; base-uri 'self'; form-action 'none'";

namespace {

constexpr UINT kPostMessage = WM_APP + 1;
constexpr int kEdge = 6;  // logical pixels at a frameless window's edge that resize it
const wchar_t* kClass = L"StarForgeWindow";
HWND g_messages = nullptr;  // for post()

std::wstring wide(const std::string& s) { return std::filesystem::u8path(s).wstring(); }
std::string utf8(const wchar_t* s) { return std::filesystem::path(s).u8string(); }

// A COM event handler around a lambda.
template <class I, class... Args>
class Handler final : public I {
public:
    explicit Handler(std::function<HRESULT(Args...)> f) : f_(std::move(f)) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            *out = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return f_(args...); }

private:
    std::function<HRESULT(Args...)> f_;
    ULONG refs_ = 1;
};

template <class T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

int scale(HWND hwnd, int logical) { return MulDiv(logical, static_cast<int>(GetDpiForWindow(hwnd)), 96); }

// The interface's background (launcher.css), for the parts of a window the webview doesn't cover.
HBRUSH background() {
    static const HBRUSH brush = CreateSolidBrush(RGB(0x0d, 0x0c, 0x0b));
    return brush;
}

}  // namespace

struct NativeWindow::Impl {
    NativeWindow* owner = nullptr;
    HWND hwnd = nullptr;
    std::unique_ptr<webview::webview> view;
    bool frameless = false, resizable = true;
    int min_width = 0, min_height = 0;
    HICON icon = nullptr;
    EventRegistrationToken resources{}, navigation{}, popups{};
    bool shown = false, start_maximized = false;  // a saved maximized state, for the first show()

    // The webview fills the window, less a resize border on a frameless one (the window gets the
    // mouse there and says which edge it is).
    void layout() {
        if (!view) return;
        auto widget = view->widget();
        if (!widget.ok()) return;
        RECT r;
        GetClientRect(hwnd, &r);
        const int inset = frameless && resizable && !IsZoomed(hwnd) ? scale(hwnd, kEdge) : 0;
        MoveWindow(static_cast<HWND>(widget.value()), inset, inset, std::max(0L, r.right - 2 * inset),
                   std::max(0L, r.bottom - 2 * inset), TRUE);
        // The resize border's width changes with maximizing, and what it uncovers was never
        // painted (white), so paint it again.
        InvalidateRect(hwnd, nullptr, TRUE);
    }
};

namespace {

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* w = reinterpret_cast<NativeWindow::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        w = static_cast<NativeWindow::Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->hwnd = hwnd;
    }
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_NCCALCSIZE:
        if (w->frameless && wp) {
            // No frame: the client area is the whole window, except that a maximized window
            // keeps to the monitor's work area instead of hanging over its edges.
            if (IsZoomed(hwnd)) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                MONITORINFO mi{sizeof mi};
                if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) p->rgrc[0] = mi.rcWork;
            }
            return 0;
        }
        break;
    case WM_NCACTIVATE:
        // -1: change the active state without painting a frame, which a frameless window doesn't have.
        if (w->frameless) return DefWindowProcW(hwnd, msg, wp, -1);
        break;
    case WM_NCHITTEST:
        if (w->frameless) {
            if (!w->resizable || IsZoomed(hwnd)) return HTCLIENT;
            POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            RECT r;
            GetWindowRect(hwnd, &r);
            const int e = scale(hwnd, kEdge);
            const bool l = pt.x < r.left + e, rt = pt.x >= r.right - e, t = pt.y < r.top + e, b = pt.y >= r.bottom - e;
            if (t && l) return HTTOPLEFT;
            if (t && rt) return HTTOPRIGHT;
            if (b && l) return HTBOTTOMLEFT;
            if (b && rt) return HTBOTTOMRIGHT;
            if (t) return HTTOP;
            if (b) return HTBOTTOM;
            if (l) return HTLEFT;
            if (rt) return HTRIGHT;
            return HTCLIENT;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = scale(hwnd, w->min_width);
        mm->ptMinTrackSize.y = scale(hwnd, w->min_height);
        return 0;
    }
    case WM_SIZE: w->layout(); return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(reinterpret_cast<HDC>(wp), &r, background());
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, background());
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (w->owner && w->owner->on_close_requested) w->owner->on_close_requested();
        return 0;  // the window stays unless the handler closes it
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK message_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kPostMessage) {
        auto* f = reinterpret_cast<std::function<void()>*>(lp);
        (*f)();
        delete f;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Serves https://starforge.localhost/<path> from the built-in interface files, with the CSP, and
// keeps the webview on those pages.
void serve_assets(ICoreWebView2Controller* controller, NativeWindow::Impl& w) {
    Com<ICoreWebView2> core;
    if (FAILED(controller->get_CoreWebView2(core.put()))) throw std::runtime_error("WebView2: no CoreWebView2");
    Com<ICoreWebView2_2> core2;
    if (FAILED(core->QueryInterface(__uuidof(ICoreWebView2_2), reinterpret_cast<void**>(core2.put()))))
        throw std::runtime_error("WebView2 is too old (needs ICoreWebView2_2)");
    ICoreWebView2Environment* env = nullptr;
    if (FAILED(core2->get_Environment(&env))) throw std::runtime_error("WebView2: no environment");
    const std::wstring filter = wide(std::string(kOrigin) + "/*");
    core->AddWebResourceRequestedFilter(filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    core->add_WebResourceRequested(
        new Handler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(
            [env](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                Com<ICoreWebView2WebResourceRequest> request;
                args->get_Request(request.put());
                LPWSTR uri_w = nullptr;
                request->get_Uri(&uri_w);
                const std::string uri = uri_w ? utf8(uri_w) : "";
                CoTaskMemFree(uri_w);
                const std::string origin = std::string(kOrigin) + "/";
                const Asset* asset = nullptr;
                if (uri.rfind(origin, 0) == 0) {
                    std::string path = uri.substr(origin.size());
                    path = path.substr(0, path.find_first_of("?#"));
                    asset = find_asset(path.empty() ? "index.html" : path);
                }
                Com<ICoreWebView2WebResourceResponse> response;
                if (!asset) {
                    env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", response.put());
                } else {
                    IStream* stream = SHCreateMemStream(asset->data, static_cast<UINT>(asset->size));
                    const std::wstring headers =
                        wide(std::string("Content-Type: ") + asset->mime + "\r\nContent-Security-Policy: " + kCsp);
                    env->CreateWebResourceResponse(stream, 200, L"OK", headers.c_str(), response.put());
                    if (stream) stream->Release();
                }
                return args->put_Response(response.p);
            }),
        &w.resources);
    // Only the interface's own pages load; links elsewhere go nowhere (the interface opens them
    // through the launcher), and nothing opens a new window.
    core->add_NavigationStarting(
        new Handler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri_w = nullptr;
                args->get_Uri(&uri_w);
                const std::string uri = uri_w ? utf8(uri_w) : "";
                CoTaskMemFree(uri_w);
                if (uri.rfind(std::string(kOrigin) + "/", 0) != 0) args->put_Cancel(TRUE);
                return S_OK;
            }),
        &w.navigation);
    core->add_NewWindowRequested(
        new Handler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { return args->put_Handled(TRUE); }),
        &w.popups);
}

}  // namespace

NativeWindow::NativeWindow(const WindowSpec& spec) : impl_(std::make_shared<Impl>()) {
    Impl& w = *impl_;
    w.owner = this;
    w.frameless = spec.frameless;
    w.resizable = spec.resizable;
    w.min_width = spec.min_width;
    w.min_height = spec.min_height;
    DWORD style = spec.frameless ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    if (spec.resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    else style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    style |= WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD ex = 0;
    if (spec.skip_taskbar) ex |= WS_EX_TOOLWINDOW;
    if (spec.always_on_top) ex |= WS_EX_TOPMOST;
    if (spec.click_through) ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
    if (spec.transparent) ex |= WS_EX_NOREDIRECTIONBITMAP;  // WebView2 composes straight onto the desktop
    const std::wstring title = wide(spec.title);
    CreateWindowExW(ex, kClass, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, spec.width, spec.height, nullptr,
                    nullptr, GetModuleHandleW(nullptr), &w);
    if (!w.hwnd) throw std::runtime_error("cannot create a window (error " + std::to_string(GetLastError()) + ")");
    if (spec.click_through) SetLayeredWindowAttributes(w.hwnd, 0, 255, LWA_ALPHA);
    if (spec.frameless) {
        // Windows 11 draws a thin border around a window with a resize frame, light in the light
        // theme; a frameless window's interface draws its own edges.
        const COLORREF none = DWMWA_COLOR_NONE;
        DwmSetWindowAttribute(w.hwnd, DWMWA_BORDER_COLOR, &none, sizeof none);
    }
    // Logical size at the window's DPI, centred on its monitor's work area if asked.
    const int width = scale(w.hwnd, spec.width), height = scale(w.hwnd, spec.height);
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    int x = 0, y = 0;
    MONITORINFO mi{sizeof mi};
    if (spec.center && GetMonitorInfoW(MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2;
    } else {
        flags |= SWP_NOMOVE;
    }
    SetWindowPos(w.hwnd, nullptr, x, y, width, height, flags | SWP_FRAMECHANGED);

    const bool debug = launch::get_env("STAR_FORGE_DEVTOOLS").has_value();
    w.view = std::make_unique<webview::webview>(debug, w.hwnd);
    auto* controller = static_cast<ICoreWebView2Controller*>(w.view->browser_controller().value());
    // What WebView2 shows before the page paints and in the strip a resize uncovers: white
    // unless set, so the interface's own background (launcher.css), or nothing.
    Com<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(controller->QueryInterface(__uuidof(ICoreWebView2Controller2), reinterpret_cast<void**>(c2.put()))))
        c2->put_DefaultBackgroundColor(spec.transparent ? COREWEBVIEW2_COLOR{0, 0, 0, 0} : COREWEBVIEW2_COLOR{255, 0x0d, 0x0c, 0x0b});
    serve_assets(controller, w);
    w.layout();
}

NativeWindow::~NativeWindow() {
    impl_->owner = nullptr;
    impl_->view.reset();
    DestroyWindow(impl_->hwnd);
    if (impl_->icon) DestroyIcon(impl_->icon);
}

void NativeWindow::init_script(const std::string& js) { impl_->view->init(js); }

void NativeWindow::bind(const std::string& name, std::function<void(const std::string&, const std::string&)> f) {
    impl_->view->bind(
        name, [f](std::string id, std::string args, void*) { f(id, args); }, nullptr);
}

void NativeWindow::resolve(const std::string& id, bool ok, const std::string& json) {
    std::weak_ptr<Impl> weak = impl_;
    // webview's own resolve() queues a call holding its engine, which may be gone by the time it
    // runs (the splash closes as it answers); this one checks the window is still there.
    post([weak, id, ok, json] {
        if (auto w = weak.lock(); w && w->view)
            w->view->eval("window.__webview__.onReply(" + webview::detail::json_escape(id) + ", " + (ok ? "0" : "1") + ", " +
                          webview::detail::json_escape(json) + ")");
    });
}

void NativeWindow::eval(const std::string& js) {
    std::weak_ptr<Impl> weak = impl_;
    post([weak, js] {
        if (auto w = weak.lock(); w && w->view) w->view->eval(js);
    });
}

void NativeWindow::load(const std::string& page) { impl_->view->navigate(std::string(kOrigin) + "/" + page); }

void NativeWindow::show() {
    HWND h = impl_->hwnd;
    const bool first = !impl_->shown;
    impl_->shown = true;
    ShowWindow(h, first && impl_->start_maximized ? SW_SHOWMAXIMIZED : IsIconic(h) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(h);
}

void NativeWindow::hide() { ShowWindow(impl_->hwnd, SW_HIDE); }
void NativeWindow::minimize() { ShowWindow(impl_->hwnd, SW_MINIMIZE); }
void NativeWindow::toggle_maximize() { ShowWindow(impl_->hwnd, IsZoomed(impl_->hwnd) ? SW_RESTORE : SW_MAXIMIZE); }

void NativeWindow::start_drag() {
    // As if the press were on a title bar (the button is still down).
    ReleaseCapture();
    POINT pt;
    GetCursorPos(&pt);
    SendMessageW(impl_->hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
}

bool NativeWindow::visible() const { return IsWindowVisible(impl_->hwnd) != FALSE; }

void NativeWindow::fit_to_monitor() {
    HWND h = impl_->hwnd;
    if (IsZoomed(h)) return;
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    const RECT a = mi.rcWork;
    RECT r;
    GetWindowRect(h, &r);
    const int w = std::min(r.right - r.left, a.right - a.left), hh = std::min(r.bottom - r.top, a.bottom - a.top);
    const int x = std::clamp(static_cast<int>(r.left), static_cast<int>(a.left), static_cast<int>(a.right) - w);
    const int y = std::clamp(static_cast<int>(r.top), static_cast<int>(a.top), static_cast<int>(a.bottom) - hh);
    if (x != r.left || y != r.top || w != r.right - r.left || hh != r.bottom - r.top)
        SetWindowPos(h, nullptr, x, y, w, hh, SWP_NOZORDER | SWP_NOACTIVATE);
}

void NativeWindow::set_icon_png(const std::string& png) {
    // Windows reads PNG icon images directly.
    HICON icon = CreateIconFromResourceEx(reinterpret_cast<PBYTE>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()),
                                          TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
    if (!icon) return;
    if (impl_->icon) DestroyIcon(impl_->icon);
    impl_->icon = icon;
    SendMessageW(impl_->hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(impl_->hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

NativeWindow::State NativeWindow::state() const {
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(impl_->hwnd, &wp);
    const RECT& r = wp.rcNormalPosition;
    return {r.left, r.top, r.right - r.left, r.bottom - r.top, IsZoomed(impl_->hwnd) || (!impl_->shown && impl_->start_maximized)};
}

void NativeWindow::set_state(const State& s) {
    WINDOWPLACEMENT wp{sizeof wp};
    GetWindowPlacement(impl_->hwnd, &wp);
    wp.rcNormalPosition = {s.x, s.y, s.x + s.width, s.y + s.height};
    wp.showCmd = SW_HIDE;  // shown later, by show()
    SetWindowPlacement(impl_->hwnd, &wp);
    impl_->start_maximized = s.maximized;
}

void NativeWindow::snapshot(const std::string& png_path) {
    auto* controller = static_cast<ICoreWebView2Controller*>(impl_->view->browser_controller().value());
    Com<ICoreWebView2> core;
    IStream* stream = nullptr;
    const std::wstring path = wide(png_path);
    if (FAILED(controller->get_CoreWebView2(core.put())) ||
        FAILED(SHCreateStreamOnFileEx(path.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream)))
        return;
    core->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream,
                         new Handler<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>([stream](HRESULT) -> HRESULT {
                             stream->Release();
                             return S_OK;
                         }));
}

void* NativeWindow::handle() const { return impl_->hwnd; }

void init() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
        throw std::runtime_error("cannot start COM");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = background();
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));  // the program's icon (app.rc)
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW mc{sizeof mc};
    mc.lpfnWndProc = message_proc;
    mc.hInstance = inst;
    mc.lpszClassName = L"StarForgeMessages";
    RegisterClassExW(&mc);
    g_messages = CreateWindowExW(0, mc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    if (!g_messages) throw std::runtime_error("cannot create the message window");
}

void run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void quit() { PostQuitMessage(0); }

void post(std::function<void()> f) {
    auto* p = new std::function<void()>(std::move(f));
    if (!PostMessageW(g_messages, kPostMessage, 0, reinterpret_cast<LPARAM>(p))) delete p;
}

namespace {

std::optional<std::string> choose(NativeWindow* parent, const std::string& title, bool folder,
                                  const std::optional<std::string>& start, const std::wstring& filter_name,
                                  const std::wstring& patterns) {
    Com<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return std::nullopt;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    const std::wstring t = wide(title);
    dialog->SetTitle(t.c_str());
    if (!patterns.empty()) {
        COMDLG_FILTERSPEC spec{filter_name.c_str(), patterns.c_str()};
        dialog->SetFileTypes(1, &spec);
    }
    if (start) {
        Com<IShellItem> item;
        const std::wstring s = wide(*start);
        if (SUCCEEDED(SHCreateItemFromParsingName(s.c_str(), nullptr, IID_PPV_ARGS(item.put())))) dialog->SetFolder(item.p);
    }
    if (FAILED(dialog->Show(parent ? static_cast<HWND>(parent->handle()) : nullptr))) return std::nullopt;  // cancelled
    Com<IShellItem> result;
    if (FAILED(dialog->GetResult(result.put()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::string out = utf8(path);
    CoTaskMemFree(path);
    return out;
}

}  // namespace

std::optional<std::string> pick_folder(NativeWindow* parent, const std::string& title, const std::optional<std::string>& start) {
    return choose(parent, title, true, start, L"", L"");
}

std::optional<std::string> pick_file(NativeWindow* parent, const std::string& title, const std::string& filter_name,
                                     const std::vector<std::string>& extensions) {
    std::wstring patterns;
    for (const auto& e : extensions) patterns += (patterns.empty() ? L"*." : L";*.") + wide(e);
    return choose(parent, title, false, std::nullopt, wide(filter_name), patterns);
}

std::optional<std::string> pick_save_file(NativeWindow* parent, const std::string& title, const std::string& filter_name,
                                          const std::string& extension, const std::optional<std::string>& start,
                                          const std::string& name) {
    Com<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dialog.put()))))
        return std::nullopt;
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    const std::wstring t = wide(title), fname = wide(filter_name), pattern = L"*." + wide(extension), ext = wide(extension),
                       n = wide(name);
    dialog->SetTitle(t.c_str());
    COMDLG_FILTERSPEC spec{fname.c_str(), pattern.c_str()};
    dialog->SetFileTypes(1, &spec);
    dialog->SetDefaultExtension(ext.c_str());  // added when the player types a name without it
    dialog->SetFileName(n.c_str());
    if (start) {
        Com<IShellItem> item;
        const std::wstring s = wide(*start);
        if (SUCCEEDED(SHCreateItemFromParsingName(s.c_str(), nullptr, IID_PPV_ARGS(item.put())))) dialog->SetFolder(item.p);
    }
    if (FAILED(dialog->Show(parent ? static_cast<HWND>(parent->handle()) : nullptr))) return std::nullopt;  // cancelled
    Com<IShellItem> result;
    if (FAILED(dialog->GetResult(result.put()))) return std::nullopt;
    PWSTR path = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    std::string out = utf8(path);
    CoTaskMemFree(path);
    return out;
}

// --- Tray: a notification-area icon, its messages going to a hidden window (a message-only one
// would miss the TaskbarCreated broadcast sent when Explorer restarts). ---

namespace {

constexpr UINT kTrayMessage = WM_APP + 2;
enum : UINT { kMenuShow = 1, kMenuExit = 2 };

class WinTray final : public Tray {
public:
    WinTray(std::function<void()> on_show, std::function<void()> on_exit) : on_show_(std::move(on_show)), on_exit_(std::move(on_exit)) {}

    ~WinTray() override {
        Shell_NotifyIconW(NIM_DELETE, &data_);
        if (hwnd_) DestroyWindow(hwnd_);
        if (icon_) DestroyIcon(icon_);
    }

    bool start(const std::string& png, const std::string& tooltip) {
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.lpszClassName = L"StarForgeTray";
        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        icon_ = CreateIconFromResourceEx(reinterpret_cast<PBYTE>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size()),
                                         TRUE, 0x00030000, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                         LR_DEFAULTCOLOR);
        data_.cbSize = sizeof data_;
        data_.hWnd = hwnd_;
        data_.uID = 1;
        data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data_.uCallbackMessage = kTrayMessage;
        data_.hIcon = icon_ ? icon_ : LoadIconW(inst, MAKEINTRESOURCEW(1));
        lstrcpynW(data_.szTip, wide(tooltip).c_str(), ARRAYSIZE(data_.szTip));
        return Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        static const UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        if (msg == WM_NCCREATE)
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<WinTray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && msg == taskbar_created) {
            Shell_NotifyIconW(NIM_ADD, &self->data_);
            return 0;
        }
        // Either button opens the menu, as show_menu_on_left_click does.
        if (self && msg == kTrayMessage && (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP)) {
            self->menu();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void menu() {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, kMenuShow, L"Show STAR FORGE Launcher");
        AppendMenuW(m, MF_STRING, kMenuExit, L"Exit");
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);  // so the menu closes when clicking elsewhere
        const UINT chosen = static_cast<UINT>(
            TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr));
        PostMessageW(hwnd_, WM_NULL, 0, 0);
        DestroyMenu(m);
        if (chosen == kMenuShow && on_show_) on_show_();
        if (chosen == kMenuExit && on_exit_) on_exit_();
    }

    std::function<void()> on_show_, on_exit_;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    NOTIFYICONDATAW data_{};
};

}  // namespace

std::unique_ptr<Tray> Tray::create(const std::string& icon_png, const std::string& tooltip, std::function<void()> on_show,
                                   std::function<void()> on_exit) {
    auto tray = std::make_unique<WinTray>(std::move(on_show), std::move(on_exit));
    if (!tray->start(icon_png, tooltip)) return nullptr;
    return tray;
}

// --- One launcher per data folder: a named mutex says one is running; it listens on a
// message-only window named after the folder, which later ones send their arguments to. ---

namespace {

const wchar_t* kInstanceClass = L"StarForgeInstance";
std::function<void(std::vector<std::string>)> g_instances;  // UI thread

std::wstring instance_name(const std::string& key) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : key) h = (h ^ c) * 1099511628211ull;
    wchar_t name[64];
    swprintf(name, 64, L"star-forge-%016llx", static_cast<unsigned long long>(h));
    return name;
}

LRESULT CALLBACK instance_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA) {
        // The arguments, each ended by a NUL.
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        const std::string message(static_cast<const char*>(cd->lpData), cd->cbData);
        std::vector<std::string> args;
        for (size_t at = 0, end; (end = message.find('\0', at)) != std::string::npos; at = end + 1)
            args.push_back(message.substr(at, end - at));
        if (g_instances) post([args] { g_instances(args); });
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool hand_over(const std::string& key, const std::vector<std::string>& args) {
    const std::wstring name = instance_name(key);
    // Held until this launcher exits.
    CreateMutexW(nullptr, FALSE, (L"Local\\" + name).c_str());
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    std::string message;
    for (const auto& arg : args) message.append(arg).push_back('\0');
    // The first launcher may still be starting: give its window a moment to appear.
    for (int i = 0; i < 50; ++i) {
        if (HWND first = FindWindowExW(HWND_MESSAGE, nullptr, kInstanceClass, name.c_str())) {
            DWORD pid = 0;
            GetWindowThreadProcessId(first, &pid);
            AllowSetForegroundWindow(pid);  // it may bring its window to the front
            COPYDATASTRUCT cd{1, static_cast<DWORD>(message.size()), message.data()};
            DWORD_PTR result;
            SendMessageTimeoutW(first, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &result);
            break;
        }
        Sleep(100);
    }
    return true;
}

void listen_for_instances(const std::string& key, std::function<void(std::vector<std::string>)> f) {
    g_instances = std::move(f);
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = instance_proc;
    wc.hInstance = inst;
    wc.lpszClassName = kInstanceClass;
    RegisterClassExW(&wc);
    CreateWindowExW(0, kInstanceClass, instance_name(key).c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
    // WM_COPYDATA comes from a lower-integrity launcher too.
    if (HWND hwnd = FindWindowExW(HWND_MESSAGE, nullptr, kInstanceClass, instance_name(key).c_str()))
        ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
}

void register_links() {
    // What tauri-plugin-deep-link does: HKCU\Software\Classes\starforge, opening this program.
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    if (n == 0 || n == ARRAYSIZE(exe)) throw std::runtime_error("cannot find this program");
    const std::wstring command = L"\"" + std::wstring(exe) + L"\" \"%1\"";
    const std::wstring name = L"URL:STAR FORGE Launcher", empty;
    auto set = [](const wchar_t* key, const wchar_t* value, const std::wstring& data) {
        const LSTATUS r = RegSetKeyValueW(HKEY_CURRENT_USER, key, value, REG_SZ, data.c_str(),
                                          static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
        if (r != ERROR_SUCCESS) throw std::runtime_error("cannot register starforge:// links (error " + std::to_string(r) + ")");
    };
    set(L"Software\\Classes\\starforge", nullptr, name);
    set(L"Software\\Classes\\starforge", L"URL Protocol", empty);
    set(L"Software\\Classes\\starforge\\shell\\open\\command", nullptr, command);
}

void open(const std::string& target) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const std::wstring t = wide(target);
    const auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", t.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    const DWORD err = GetLastError();
    if (SUCCEEDED(com)) CoUninitialize();
    if (r <= 32) throw std::runtime_error("could not open " + target + ": error " + std::to_string(r <= 32 && err ? err : r));
}

}  // namespace sf::app
