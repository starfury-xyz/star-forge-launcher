// Loads the server proxy (libcrypto-1_1_tg.dll) into this process the way a server exe loads it,
// then checks what it did. Built into <build>/fakeserver/cloud/ next to the proxy, so the proxy
// sees cloud\starforge\ beside "its exe" and <build>/fakeserver as the game folder. x86 only,
// like the servers; runs under wine via ctest.
//
// Usage: proxy_test <scenario>. One process per scenario (hooks are for life, like in a server),
// and the scenarios run one at a time: they share cloud\starforge\.
//
// The patch scenarios patch g_marker in this exe's own image, with a patchset made for this
// very build (its sha1, its image base).

#include "common/file.hpp"
#include "common/text.hpp"
#include "patch/patchset.hpp"
#include "patch/sha1.hpp"
#include "test_util.hpp"

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <tuple>
#include <vector>

using sf::utf8;
using test::check;
using test::contains;

// What the patches change: 8 bytes in this exe's data.
extern "C" volatile unsigned char g_marker[8] = {'u', 'n', 'p', 'a', 't', 'c', 'h', 'd'};

// Midhook sites (see proxy_test_hooks.cpp): return 1 unless hooked. Naked, so the bytes are known.
extern "C" __declspec(naked) int hook_site_add() {
    __asm {
        xor ecx, ecx
        mov eax, 1  // +2: the site, B8 01 00 00 00
        add eax, ecx
        ret
    }
}
extern "C" __declspec(naked) int hook_site_jump() {
    __asm {
        xor ecx, ecx
        mov eax, 1
        add eax, ecx
        ret
        nop  // not identical to hook_site_add, so the linker can't fold the two
    }
}
extern "C" __declspec(dllexport) int jump_target() { return 99; }

namespace {

std::wstring g_cloud, g_game, g_root, g_dll, g_log;

// Loads the proxy as a server exe would (its DllMain runs). Returns its log.
std::string load_proxy(bool loaded_expected = true) {
    HMODULE m = LoadLibraryW(g_dll.c_str());
    check((m != nullptr) == loaded_expected, loaded_expected ? "proxy loads" : "proxy refuses to load (the server must not start)");
    return test::read_file(g_log);
}

std::string marker() { return std::string(reinterpret_cast<const char*>(const_cast<unsigned char*>(g_marker)), 8); }

// A patchset for this exe: one patch per (id, new bytes); `extra` goes into each [[patch]].
std::string patchset(const std::string& sha1, const std::string& extra = "", const std::string& write_extra = "") {
    std::string exe;
    std::wstring self(1024, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    sf::read_file(self, exe);
    const auto image = sf::patch::FileImage::parse(sf::patch::Bytes(exe.begin(), exe.end()));
    const uint64_t rva = reinterpret_cast<uintptr_t>(g_marker) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const std::string orig = sf::patch::to_hex(sf::patch::Bytes(g_marker, g_marker + 8));
    return "format = 1\nexe = \"proxy_test.exe\"\nsha1 = \"" + (sha1.empty() ? sf::patch::sha1_hex(exe.data(), exe.size()) : sha1) +
           "\"\nimage_base = " + sf::patch::hex_va(image.image_base) + "\n\n[[patch]]\nid = \"marker\"\ntitle = \"Marker\"\n" +
           extra + "[[patch.write]]\nva = " + sf::patch::hex_va(image.image_base + rva) + "\norig = \"" + orig + "\"\n" +
           (write_extra.empty() ? "bytes = \"" + sf::patch::to_hex(sf::patch::Bytes{'p', 'a', 't', 'c', 'h', 'e', 'd', '!'}) + "\"\n"
                                : write_extra);
}

void write_patchset(const std::string& text) { test::write_file(g_root + L"\\patchsets\\proxy_test.exe.patchset.toml", text); }

// A file read the way a server reads it; "(missing)" if it can't be opened.
std::string read_a(const std::wstring& path) {
    HANDLE h = CreateFileA(utf8(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "(missing)";
    char buf[256];
    DWORD n = 0;
    ReadFile(h, buf, sizeof buf, &n, nullptr);
    CloseHandle(h);
    return std::string(buf, n);
}

// Appends to a file (opening it for writing, OPEN_ALWAYS), the way a server writes a log or cache.
bool append_a(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileA(utf8(path).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD n = 0;
    const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr) && n == text.size();
    CloseHandle(h);
    return ok;
}

// Runs an exe in cloud\ with `args`; returns its stdout, and its exit code in `code`.
std::string run(const std::wstring& exe, const std::wstring& args, DWORD* code = nullptr) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "(no pipe)";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    const std::wstring path = g_cloud + L"\\" + exe;
    std::wstring cmd = L"\"" + path + L"\" " + args;
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    const bool started = CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    std::string out;
    char buf[256];
    DWORD n = 0;
    while (started && ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(rd);
    if (!started) return "(child failed)";
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (code) GetExitCodeProcess(pi.hProcess, code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return out;
}

// What is really on disk at `path`, past the hooks: asked of a child process (this exe, `cat`
// mode) that has no proxy loaded. "(missing)" if there's no such file.
std::string on_disk(const std::wstring& path) { return run(L"proxy_test.exe", L"cat \"" + path + L"\""); }
bool disk_has(const std::wstring& path) { return on_disk(path) != "(missing)"; }

// `proxy_test cat <path>`: prints the file, or "(missing)" (a folder counts as there).
int cat(const std::wstring& path) {
    _setmode(_fileno(stdout), _O_BINARY);  // the bytes as they are, no \n -> \r\n
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) std::fputs("(folder)", stdout);
    else if (!std::filesystem::exists(path, ec)) std::fputs("(missing)", stdout);
    else std::fputs(test::read_file(path).c_str(), stdout);
    return 0;
}

// Every name a listing returns, sorted, via FindFirstFileExA + FindNextFileA as the servers list.
std::string list(const std::wstring& pattern) {
    WIN32_FIND_DATAA d = {};
    HANDLE h = FindFirstFileExA(utf8(pattern).c_str(), FindExInfoBasic, &d, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return "(none)";
    std::vector<std::string> names;
    do {
        const std::string n = d.cFileName;
        if (n != "." && n != "..") names.push_back(n);
    } while (FindNextFileA(h, &d));
    FindClose(h);
    std::sort(names.begin(), names.end());
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : " ") + n;
    return out;
}

// --- scenarios -------------------------------------------------------------------------------

// No cloud\starforge\: the proxy is just the original DLL.
void plain() {
    std::filesystem::remove_all(g_root);
    check(LoadLibraryW(g_dll.c_str()) != nullptr, "loads without starforge\\");
    check(!std::filesystem::exists(g_root), "creates nothing");
    check(marker() == "unpatchd", "patches nothing");
}

std::string patch() {
    write_patchset(patchset(""));
    std::string text = load_proxy();
    check(marker() == "patched!", "the patch is applied in memory");
    check(contains(text, "star-forge proxy " SF_VERSION " in proxy_test.exe"), "log: version and exe");
    check(contains(text, "applied marker (1 writes)"), "log: the patch");
    check(contains(text, "1/1 patches applied (1 in patchset)"), "log: the count");
    check(contains(text, "no overlay folder"), "log: no overlay");
    return text;
}

std::string patch_off() {
    write_patchset(patchset(""));
    test::write_file(g_root + L"\\patcher.toml", "[patches.\"PROXY_TEST.EXE\"]\nmarker = false\n");
    std::string text = load_proxy();
    check(marker() == "unpatchd", "switched off in patcher.toml (exe name in any case): not applied");
    check(contains(text, "0/0 patches applied (1 in patchset)"), "log: nothing on");
    return text;
}

std::string wrong_build() {
    write_patchset(patchset("0000000000000000000000000000000000000000"));
    std::string text = load_proxy(false);
    check(marker() == "unpatchd", "nothing applied");
    check(contains(text, "FATAL: proxy_test.exe is a different build"), "log: different build");
    check(contains(text, "refusing to start"), "log: refusing to start");
    return text;
}

std::string wrong_build_continue() {
    write_patchset(patchset("0000000000000000000000000000000000000000"));
    test::write_file(g_root + L"\\patcher.toml", "on_error = \"continue\"\n");
    std::string text = load_proxy();
    check(marker() == "unpatchd", "nothing applied");
    check(contains(text, "ERROR: proxy_test.exe is a different build"), "log: the problem, as an ERROR");
    return text;
}

std::string bad_config() {
    test::write_file(g_root + L"\\patcher.toml", "on_error = \"maybe\"\n");
    std::string text = load_proxy(false);
    check(contains(text, "FATAL: patcher.toml: on_error must be"), "log: the config error");
    return text;
}

std::string midhook() {
    write_patchset(patchset("", "optional = true\n", "midhook = \"nope\"\n"));
    std::string text = load_proxy();
    check(contains(text, "skipping optional patch marker: midhook 'nope' is not compiled into this DLL"),
          "an optional midhook patch is skipped");
    return text;
}

// A patchset with one midhook patch per (id, hook, site function); the site is the function + 2.
std::string midhook_patchset(const std::vector<std::tuple<std::string, std::string, int (*)()>>& hooks) {
    std::string exe;
    std::wstring self(1024, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size())));
    sf::read_file(self, exe);
    const auto image = sf::patch::FileImage::parse(sf::patch::Bytes(exe.begin(), exe.end()));
    std::string out = "format = 1\nexe = \"proxy_test.exe\"\nsha1 = \"" + sf::patch::sha1_hex(exe.data(), exe.size()) +
                      "\"\nimage_base = " + sf::patch::hex_va(image.image_base) + "\n";
    for (const auto& [id, hook, fn] : hooks) {
        const uint64_t rva = reinterpret_cast<uintptr_t>(fn) + 2 - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        out += "\n[[patch]]\nid = \"" + id + "\"\ntitle = \"" + id + "\"\n[[patch.write]]\nva = " +
               sf::patch::hex_va(image.image_base + rva) + "\norig = \"b801000000\"\nmidhook = \"" + hook + "\"\n";
    }
    return out;
}

// Midhooks compiled into the proxy run: one continues with a changed register, one jumps away.
std::string midhook_run() {
    check(hook_site_add() == 1 && hook_site_jump() == 1, "the sites return 1 unhooked");
    write_patchset(midhook_patchset({{"add", "sf_test_add", hook_site_add}, {"jump", "sf_test_jump", hook_site_jump}}));
    std::string text = load_proxy();
    check(contains(text, "applied add (1 writes)") && contains(text, "applied jump (1 writes)"), "log: both applied");
    check(hook_site_add() == 42, "the hook's ecx reaches the code after the stolen bytes");
    check(hook_site_add() == 42, "... every call");
    check(hook_site_jump() == 99, "a hook's return address is jumped to");
    text = test::read_file(g_log);
    check(contains(text, "sf_test_add ran"), "sfp::log writes to the exe's log");
    return text;
}

// The overlay: reads come from it, writes go to the state folder, the game and overlay stay.
std::string files() {
    const std::wstring overlay = g_root + L"\\overlay", state = g_root + L"\\state";
    const std::wstring cfg = g_game + L"\\cloud\\data\\proxy_test", lua = g_game + L"\\data\\gamedata\\proxy_test";
    std::filesystem::remove_all(cfg);
    std::filesystem::remove_all(lua);
    test::write_file(cfg + L"\\both.cfg", "game");
    test::write_file(cfg + L"\\game_only.cfg", "game only");
    test::write_file(cfg + L"\\log.txt", "game log\n");
    test::write_file(overlay + L"\\cloud\\data\\proxy_test\\both.cfg", "overlay");
    test::write_file(overlay + L"\\cloud\\data\\proxy_test\\overlay_only.cfg", "overlay only");
    test::write_file(lua + L"\\a.lua", "game lua");
    test::write_file(overlay + L"\\data\\gamedata\\proxy_test\\a.lua", "overlay lua");
    test::write_file(overlay + L"\\cloud\\starforge\\patcher.toml", "on_error = \"maybe\"\n");  // must never be read

    std::string text = load_proxy();
    check(contains(text, "file hooks installed"), "log: file hooks installed");
    check(contains(text, "overlay: " + utf8(overlay)), "log: the overlay folder");

    // Reads.
    check(read_a(cfg + L"\\both.cfg") == "overlay", "overlay copy wins over the game's");
    check(read_a(cfg + L"\\game_only.cfg") == "game only", "a file the overlay doesn't have: the game's");
    check(read_a(cfg + L"\\overlay_only.cfg") == "overlay only", "a file only in the overlay: opens");
    check(read_a(lua + L"\\a.lua") == "overlay lua", "data\\ (outside cloud\\) is covered too");
    check(read_a(g_root + L"\\patcher.toml") == "(missing)", "cloud\\starforge\\ is never redirected");
    check(list(cfg + L"\\*") == "both.cfg game_only.cfg log.txt overlay_only.cfg", "a listing merges game and overlay");

    // Writes.
    check(append_a(cfg + L"\\log.txt", "server line\n"), "a server can write a game file");
    check(on_disk(state + L"\\cloud\\data\\proxy_test\\log.txt") == "game log\nserver line\n",
          "... the write lands in a state copy of it");
    check(on_disk(cfg + L"\\log.txt") == "game log\n", "... the game's file is unchanged");
    check(read_a(cfg + L"\\log.txt") == "game log\nserver line\n", "... and reading it back gives the state copy");
    check(append_a(cfg + L"\\both.cfg", "+"), "a server can write an overlay file");
    check(on_disk(state + L"\\cloud\\data\\proxy_test\\both.cfg") == "overlay+", "... copied from the overlay first");
    check(on_disk(overlay + L"\\cloud\\data\\proxy_test\\both.cfg") == "overlay", "... the overlay is unchanged");
    check(append_a(cfg + L"\\new.txt", "new"), "a new file under the game folder");
    check(disk_has(state + L"\\cloud\\data\\proxy_test\\new.txt") && !disk_has(cfg + L"\\new.txt"), "... is made in the state folder");
    check(list(cfg + L"\\*") == "both.cfg game_only.cfg log.txt new.txt overlay_only.cfg", "... and listed");

    check(CreateDirectoryA(utf8(cfg + L"\\sub").c_str(), nullptr), "CreateDirectoryA: a new folder");
    check(disk_has(state + L"\\cloud\\data\\proxy_test\\sub") && !disk_has(cfg + L"\\sub"), "... is made in the state folder");
    check(!CreateDirectoryA(utf8(cfg + L"\\sub").c_str(), nullptr) && GetLastError() == ERROR_ALREADY_EXISTS,
          "... and then exists");

    check(DeleteFileA(utf8(cfg + L"\\both.cfg").c_str()), "DeleteFileA on a written file");
    check(read_a(cfg + L"\\both.cfg") == "overlay", "... drops the state copy: the overlay's shows again");
    check(!DeleteFileA(utf8(cfg + L"\\game_only.cfg").c_str()), "DeleteFileA on a game file is refused");
    check(disk_has(cfg + L"\\game_only.cfg"), "... and it stays");

    check(MoveFileA(utf8(cfg + L"\\game_only.cfg").c_str(), utf8(cfg + L"\\moved.cfg").c_str()), "MoveFileA from a game file");
    check(read_a(cfg + L"\\moved.cfg") == "game only" && on_disk(state + L"\\cloud\\data\\proxy_test\\moved.cfg") == "game only",
          "... the new name is in the state folder");
    check(disk_has(cfg + L"\\game_only.cfg"), "... the original stays (copied, not moved)");
    check(CopyFileA(utf8(cfg + L"\\overlay_only.cfg").c_str(), utf8(cfg + L"\\copy.cfg").c_str(), TRUE), "CopyFileA");
    check(on_disk(state + L"\\cloud\\data\\proxy_test\\copy.cfg") == "overlay only", "... copies what the server sees");

    text = test::read_file(g_log);
    check(contains(text, "redirect cloud\\data\\proxy_test\\both.cfg -> " + utf8(overlay)), "log: the redirect");
    check(contains(text, "write cloud\\data\\proxy_test\\log.txt -> " + utf8(state)), "log: the copy on write");
    check(contains(text, "delete cloud\\data\\proxy_test\\game_only.cfg: refused"), "log: the refused delete");
    return text;
}

// [files] with an absolute overlay folder (e.g. a starfury checkout's game\).
std::string files_elsewhere() {
    const std::wstring checkout = test::dir_of(g_game) + L"\\proxy_checkout\\game";
    const std::wstring cfg = g_game + L"\\cloud\\data\\proxy_test";
    std::filesystem::remove_all(cfg);
    test::write_file(cfg + L"\\both.cfg", "game");
    test::write_file(checkout + L"\\cloud\\data\\proxy_test\\both.cfg", "checkout");
    std::wstring setting = checkout;
    for (auto& c : setting)
        if (c == L'\\') c = L'/';
    test::write_file(g_root + L"\\patcher.toml", "[files]\noverlay = '" + utf8(setting) + "'\n");
    std::string text = load_proxy();
    check(read_a(cfg + L"\\both.cfg") == "checkout", "the overlay is read from the configured folder");
    return text;
}

// [stack]: stack_test.exe (which imports the proxy, like a server exe) gets a 16 MB main thread
// and survives using 6 MB of it.
std::string stack() {
    test::write_file(g_root + L"\\patcher.toml", "[stack]\n\"stack_test.exe\" = 16\n");
    DWORD code = 0;
    const std::string out = run(L"stack_test.exe", L"deep", &code);
    check(contains(out, "deep ok"), "6 MB of recursion survives");
    check(contains(out, "stack 16777216"), "the main thread has 16 MB");
    check(code == 7, "the exit code comes through");
    const std::string text = test::read_file(g_root + L"\\logs\\stack_test.log");
    check(contains(text, "main thread stack: 16 MB"), "log: the stack");
    return out + text;
}

// No [stack]: the exe's own stack, and the proxy leaves the entry point alone.
std::string stack_off() {
    test::write_file(g_root + L"\\patcher.toml", "[stack]\n\"Other.exe\" = 16\n");
    DWORD code = 0;
    const std::string out = run(L"stack_test.exe", L"", &code);
    check(contains(out, "stack 1048576"), "the main thread keeps the exe's 1 MB");
    check(code == 7, "the exit code comes through");
    const std::string text = test::read_file(g_root + L"\\logs\\stack_test.log");
    check(!contains(text, "main thread stack"), "log: no stack change");
    return out + text;
}

}  // namespace

int main(int argc, char** argv) {
    test::exit_on_crash();
    const std::string scenario = argc > 1 ? argv[1] : "";
    if (scenario == "cat" && argc > 2) return cat(sf::wide(argv[2]));
    g_cloud = test::exe_dir();
    g_game = test::dir_of(g_cloud);
    g_root = g_cloud + L"\\starforge";
    g_dll = g_cloud + L"\\libcrypto-1_1_tg.dll";
    g_log = g_root + L"\\logs\\proxy_test.log";
    if (scenario != "plain") test::fresh_dir(g_root);

    std::string text;
    if (scenario == "plain") plain();
    else if (scenario == "patch") text = patch();
    else if (scenario == "patch_off") text = patch_off();
    else if (scenario == "wrong_build") text = wrong_build();
    else if (scenario == "wrong_build_continue") text = wrong_build_continue();
    else if (scenario == "bad_config") text = bad_config();
    else if (scenario == "midhook") text = midhook();
    else if (scenario == "midhook_run") text = midhook_run();
    else if (scenario == "files") text = files();
    else if (scenario == "files_elsewhere") text = files_elsewhere();
    else if (scenario == "stack") text = stack();
    else if (scenario == "stack_off") text = stack_off();
    else {
        std::printf("unknown scenario '%s'\n", scenario.c_str());
        return 2;
    }
    if (test::failures) std::printf("--- proxy log ---\n%s", text.c_str());
    return test::failures ? 1 : 0;
}
