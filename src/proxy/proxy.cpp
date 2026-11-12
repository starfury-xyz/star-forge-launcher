// Stand-in for the game's libcrypto-1_1_tg.dll, which every server exe (cloud\*.exe, 32-bit)
// imports. All its exports forward to the renamed original, libcrypto-1_1_tg_orig.dll
// (libcrypto-1_1_tg.def). Port of star-forge/patcher's proxy DLL, on sf_patch.
//
// On process attach, before the exe's own startup code runs, it reads cloud\starforge\:
//   1. patcher.toml: on_error, the per-patch switches and where changed files come from;
//   2. patchsets\<Exe>.patchset.toml: checks the exe's sha1 and every enabled patch's original
//      bytes, then applies them in memory (the exe on disk is never changed). Midhooks are C++
//      compiled into this DLL (sfp_hook.hpp, cmake -DSF_PROXY_HOOKS);
//   3. the overlay folder: hooks the file API so game files the overlay has are read from it,
//      and whatever the server writes over game files lands in the state folder.
//   4. [stack]: runs the exe on a main thread with a bigger stack, if patcher.toml asks for one.
// Each step logs to starforge\logs\<Exe>.log. A problem stops the process from starting unless
// on_error = "continue" (fail closed: a server never runs half patched by accident).
// Without a starforge\ folder it does nothing at all: just the original DLL.

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/file.hpp"
#include "common/text.hpp"
#include "patch/patchset.hpp"
#include "patch/server_config.hpp"
#include "patch/sha1.hpp"
#include "hook/hook.hpp"
#include "profile/profile.hpp"
#include "proxy/midhook.hpp"
#include "redirect/redirect.hpp"

namespace sf::proxy {
namespace {

FILE* g_log = nullptr;

void vlog(const char* fmt, va_list ap) {
    char msg[2048];
    vsnprintf(msg, sizeof msg, fmt, ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    FILE* f = g_log ? g_log : stderr;
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d [%lu] %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, GetCurrentProcessId(), msg);
    fflush(f);
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool is_dir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// A [files] folder: absolute (C:\..., \\server\...) as written, else relative to starforge\.
std::wstring folder(const std::wstring& root, const std::string& setting) {
    std::wstring w = profile::normalize(wide(setting));
    const bool absolute = (w.size() > 1 && w[1] == L':') || w.rfind(L"\\\\", 0) == 0;
    return absolute ? w : root + L"\\" + w;
}

// The exe's own image in memory, addressed like its patchset.
class Memory : public patch::Target {
public:
    Memory(uint8_t* base, uint64_t image_base, uint64_t size) : base_(base), image_base_(image_base), size_(size) {}

    bool read(uint64_t va, uint8_t* out, size_t n) override {
        uint8_t* p = at(va, n);
        if (!p) return false;
        std::memcpy(out, p, n);
        return true;
    }

    bool write(uint64_t va, const uint8_t* data, size_t n) override {
        uint8_t* p = at(va, n);
        if (!p) return false;
        DWORD old = 0;
        if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
        std::memcpy(p, data, n);
        VirtualProtect(p, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, n);
        return true;
    }

    // Where `va` is in this process, or nullptr if the image doesn't have n bytes there.
    uint8_t* at(uint64_t va, size_t n) const {
        if (va < image_base_) return nullptr;
        const uint64_t rva = va - image_base_;
        if (rva + n > size_) return nullptr;
        return base_ + rva;
    }

private:
    uint8_t* base_;
    uint64_t image_base_, size_;
};

uint64_t image_size(const uint8_t* base) {
    uint32_t nt_offset, size;
    std::memcpy(&nt_offset, base + 0x3c, 4);
    std::memcpy(&size, base + nt_offset + 0x50, 4);
    return size;
}

bool eq_ignore_ascii_case(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

// patcher.toml's switches for this exe (module names compared ignoring case, like Windows).
const patch::Choices* choices(const patch::ServerConfig& cfg, const std::string& exe) {
    for (const auto& entry : cfg.patches)
        if (eq_ignore_ascii_case(entry.first, exe)) return &entry.second;
    return nullptr;
}

// Applies this exe's patchset, if there is one. Returns "" or the failure.
std::string patch_exe(const std::wstring& root, const std::wstring& exe_path, const std::string& exe,
                      const patch::ServerConfig& cfg) {
    const std::wstring ps_path = root + L"\\patchsets\\" + wide(exe) + wide(patch::kSuffix);
    std::string text;
    if (read_file(ps_path, text) != 0) {
        log("no patchset for %s; running unpatched", exe.c_str());
        return "";
    }
    patch::Patchset set;
    try {
        set = patch::Patchset::parse(text, exe + patch::kSuffix);
    } catch (const patch::Error& e) {
        return e.what();
    }
    std::string image;
    if (unsigned long err = read_file(exe_path, image)) return "cannot read own exe for hashing: " + win_error(err);
    const std::string sha = patch::sha1_hex(image.data(), image.size());
    if (!set.accepts(sha))
        return exe + " is a different build (sha1 " + sha + ", this patchset needs " + set.expected_sha1() + ")";

    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    Memory mem(base, set.image_base, image_size(base));
    patch::Enabled en = set.enabled(choices(cfg, exe));
    for (const auto& w : en.warnings) log("warning: %s", w.c_str());
    if (auto o = patch::overlaps(en.on); !o.empty()) return "enabled patches overlap: " + o.front();

    // Verify everything before writing anything, so a bad patchset never half-applies.
    std::vector<const patch::Patch*> go;
    std::string failed;
    for (const patch::Patch* p : en.on) {
        patch::State s = patch::check(mem, *p);
        std::string why;
        if (s.kind == patch::State::Applied) why = "already applied in memory";
        else if (s.kind == patch::State::Mismatch) why = s.why;
        else
            for (const auto& w : p->writes)
                if (!w.midhook.empty() && !sfp::find_midhook(w.midhook.c_str())) {
                    why = "midhook '" + w.midhook + "' is not compiled into this DLL";
                    break;
                }
        if (why.empty()) go.push_back(p);
        else if (p->optional) log("skipping optional patch %s: %s", p->id.c_str(), why.c_str());
        else failed += (failed.empty() ? "" : "; ") + p->id + ": " + why;
    }
    if (!failed.empty()) return failed;

    // Midhook sites move with the image, like every other write (the hooks' own addresses don't).
    auto midhook = [&mem](const patch::Write& w) -> std::string {
        uint8_t* site = mem.at(w.va, w.orig.size());
        if (!site) return "site " + patch::hex_va(w.va) + " is outside the image";
        return sfp::install_midhook(uint32_t(uintptr_t(site)), w.orig.data(), w.orig.size(), sfp::find_midhook(w.midhook.c_str()));
    };
    for (const patch::Patch* p : go) {
        if (std::string err = patch::apply(mem, *p, midhook); !err.empty()) return p->id + ": " + err;
        log("applied %s (%zu writes)", p->id.c_str(), p->writes.size());
    }
    log("%zu/%zu patches applied (%zu in patchset)", go.size(), en.on.size(), set.patches.size());
    return "";
}

// Serves changed game files from the overlay, if there is one. Returns "" or the failure.
std::string serve_files(const std::wstring& root, const std::wstring& cloud, const patch::ServerConfig& cfg) {
    const std::wstring overlay = folder(root, cfg.overlay), state = folder(root, cfg.state);
    if (!is_dir(overlay)) {
        log("no overlay folder (%s): game files as installed", utf8(overlay).c_str());
        return "";
    }
    // cloud\ is <game>\cloud\: the overlay is laid out like <game>, so it covers data\ too.
    redirect::Config files;
    files.game_root = cloud.substr(0, cloud.rfind(L'\\'));
    files.layers = {overlay};
    files.writes = state;
    files.skip = {L"cloud\\starforge"};
    files.log = [](const char* line) { log("%s", line); };
    log("overlay: %s", utf8(overlay).c_str());
    log("writes:  %s", utf8(state).c_str());
    return redirect::install(std::move(files));
}

// --- a bigger main thread stack ------------------------------------------------------------------

// The exe's entry point (mainCRTStartup), via MinHook's trampoline.
using EntryFn = int(WINAPI*)();
EntryFn g_entry = nullptr;
SIZE_T g_stack = 0;

DWORD WINAPI run_entry(void*) { return DWORD(g_entry()); }

// Runs in place of the exe's entry point: the exe's whole life (CRT startup, main, exit) on a
// thread with the stack it needs. The process ends when the exe calls exit(), as it would anyway.
int WINAPI big_stack_entry() {
    HANDLE t = CreateThread(nullptr, g_stack, run_entry, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t) return g_entry();  // can't: run as before
    WaitForSingleObject(t, INFINITE);
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    ExitProcess(code);
}

// The stock exes reserve 1 MB for the main thread (SizeOfStackReserve), fixed when Windows
// creates the process. Instead of patching that header on disk, run the entry point on a new
// thread with `mb` MB. Returns "" or the failure.
std::string big_stack(int mb) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    uint32_t nt_offset, entry_rva;
    std::memcpy(&nt_offset, base + 0x3c, 4);
    std::memcpy(&entry_rva, base + nt_offset + 0x28, 4);  // OptionalHeader.AddressOfEntryPoint
    g_stack = SIZE_T(mb) << 20;
    if (std::string err = hook::install(reinterpret_cast<EntryFn>(base + entry_rva), big_stack_entry, &g_entry); !err.empty())
        return "hooking the entry point: " + err;
    log("main thread stack: %d MB", mb);
    return "";
}

}  // namespace

// Returns false when the process must not start.
bool run() {
    std::wstring exe_path(1024, L'\0');
    exe_path.resize(GetModuleFileNameW(nullptr, exe_path.data(), static_cast<DWORD>(exe_path.size())));
    exe_path = profile::normalize(exe_path);
    const size_t slash = exe_path.rfind(L'\\');
    const std::wstring cloud = exe_path.substr(0, slash), exe_w = exe_path.substr(slash + 1);
    const std::wstring root = cloud + L"\\starforge";
    if (!is_dir(root)) return true;  // not configured: behave as the plain original DLL

    CreateDirectoryW((root + L"\\logs").c_str(), nullptr);
    const std::string exe = utf8(exe_w);
    g_log = _wfopen((root + L"\\logs\\" + exe_w.substr(0, exe_w.rfind(L'.')) + L".log").c_str(), L"ab");
    log("star-forge proxy %s in %s", SF_VERSION, exe.c_str());
    log("cmdline: %s", utf8(GetCommandLineW()).c_str());

    patch::ServerConfig cfg;
    std::string text;
    const std::wstring cfg_path = root + L"\\patcher.toml";
    if (exists(cfg_path)) {
        try {
            if (unsigned long err = read_file(cfg_path, text)) throw std::runtime_error("patcher.toml: " + win_error(err));
            cfg = patch::ServerConfig::parse(text, "patcher.toml");
        } catch (const std::exception& e) {
            log("FATAL: %s", e.what());
            return false;
        }
    }
    // Logs a problem; whether the process may still start is on_error's call.
    auto may_continue = [&](const std::string& msg) {
        log("%s: %s", cfg.exit_on_error ? "FATAL" : "ERROR", msg.c_str());
        if (cfg.exit_on_error) log("refusing to start (on_error = \"exit\" in patcher.toml)");
        return !cfg.exit_on_error;
    };

    if (std::string err = patch_exe(root, exe_path, exe, cfg); !err.empty() && !may_continue(err)) return false;
    if (std::string err = serve_files(root, cloud, cfg); !err.empty() && !may_continue("file overlay: " + err)) return false;
    for (const auto& [module, mb] : cfg.stack_mb)
        if (eq_ignore_ascii_case(module, exe))
            if (std::string err = big_stack(mb); !err.empty() && !may_continue(err)) return false;
    return true;
}

}  // namespace sf::proxy

// Marker export: `sfp install` and the launcher tell this proxy from the original DLL by it.
// sfp_hook.hpp's log, for midhooks: into this exe's log.
void sfp::log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sf::proxy::vlog(fmt, ap);
    va_end(ap);
}

extern "C" __declspec(dllexport) const char* __cdecl sfp_version() { return SF_VERSION; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        try {
            return sf::proxy::run() ? TRUE : FALSE;
        } catch (...) {
            return FALSE;
        }
    }
    return TRUE;
}
