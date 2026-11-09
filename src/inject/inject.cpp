// Port of star-forge crates/sc-inject/src/main.rs.

#include "inject/inject.hpp"

#include <windows.h>

#include <tlhelp32.h>

#include <filesystem>
#include <optional>
#include <stdexcept>

#include "common/file.hpp"
#include "common/text.hpp"
#include "patch/patchset.hpp"
#include "profile/profile.hpp"

namespace sf::inject {
namespace {

// Why the game can't be started; run() says it and exits with 1.
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string last_error(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

HANDLE g_log = INVALID_HANDLE_VALUE;  // <profile>\inject.log, once the profile is known

void write_all(HANDLE h, const std::string& text) {
    DWORD n = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
}

// Prints a line and appends it to inject.log. This is a windowed program, so stdout only goes
// somewhere when the parent passed one (Proton swallows it; the log is what players send us).
void say(const std::string& msg) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) write_all(out, "sc-inject: " + msg + "\n");
    if (g_log != INVALID_HANDLE_VALUE) write_all(g_log, msg + "\n");
}

struct Args {
    std::wstring game, profile;
    std::optional<std::wstring> dll;
    std::optional<std::vector<std::wstring>> game_args;  // everything after "--", if given
};

Args parse_args(const std::vector<std::wstring>& in) {
    std::optional<std::wstring> game, profile, dll;
    Args a;
    for (size_t i = 0; i < in.size(); ++i) {
        // An option's value is the next argument; a missing one counts as not given.
        auto value = [&]() -> std::optional<std::wstring> {
            if (i + 1 < in.size()) return in[++i];
            return std::nullopt;
        };
        if (in[i] == L"--game") game = value();
        else if (in[i] == L"--profile") profile = value();
        else if (in[i] == L"--dll") dll = value();
        else if (in[i] == L"--") {
            a.game_args.emplace(in.begin() + i + 1, in.end());
            break;
        } else throw Failure("unknown argument \"" + utf8(in[i]) + "\"");
    }
    if (!game) throw Failure("--game is required");
    if (!profile) throw Failure("--profile is required");
    a.game = profile::windows_path_from_arg(*game);
    a.profile = profile::windows_path_from_arg(*profile);
    if (dll) a.dll = profile::windows_path_from_arg(*dll);
    return a;
}

bool is_file(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    std::wstring p(buf, n);
    return p.substr(0, p.rfind(L'\\'));
}

// Whether `process` has a module loaded from `path` (compared ignoring case).
bool has_module(HANDLE process, const std::wstring& path) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(process));
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = CompareStringOrdinal(me.szExePath, -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
    CloseHandle(snap);
    return found;
}

// Classic LoadLibraryW injection into the suspended game: write the DLL's path into the game's
// memory, then start a thread there that calls LoadLibraryW on it. kernel32 is at the same
// address in every process (per boot), so our LoadLibraryW is the game's too.
void inject(HANDLE process, const std::wstring& dll) {
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
    if (!load) throw Failure(last_error("GetProcAddress(LoadLibraryW)"));

    void* mem = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) throw Failure(last_error("VirtualAllocEx"));
    std::string err;
    if (!WriteProcessMemory(process, mem, dll.c_str(), bytes, nullptr)) {
        err = last_error("WriteProcessMemory");
    } else if (HANDLE thread = CreateRemoteThread(process, nullptr, 0, load, mem, 0, nullptr); !thread) {
        err = last_error("CreateRemoteThread");
    } else {
        DWORD waited = WaitForSingleObject(thread, 30000);
        DWORD module = 0;
        GetExitCodeThread(thread, &module);
        CloseHandle(thread);
        // The thread's exit code is LoadLibraryW's result cut to its low 32 bits; 0 means it
        // failed, unless the DLL happened to load at an address like 0x200000000, so then
        // look in the game's module list before calling it a failure. (The Rust injector
        // trusts the 0.)
        if (waited != WAIT_OBJECT_0)
            err = "timed out waiting for LoadLibraryW in the game";
        else if (module == 0 && !has_module(process, dll))
            err = "the agent (" + utf8(dll) +
                  ") did not load; if the pack has patches, they may not match this game build (see agent.log in "
                  "the profile folder)";
    }
    // Only after the thread is done with it (on a timeout it may still read it later: leave it).
    if (err.rfind("timed out", 0) != 0) VirtualFreeEx(process, mem, 0, MEM_RELEASE);
    if (!err.empty()) throw Failure(err);
}

int launch(const std::vector<std::wstring>& argv) {
    const Args args = parse_args(argv);
    const std::wstring profile_dir = profile::normalize(args.profile);
    // Like the Rust: no log if the profile folder doesn't exist; the load below then says why.
    g_log = CreateFileW((profile_dir + L"\\" + profile::kInjectLog).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    profile::Profile prof;
    try {
        prof = profile::load(profile_dir);
    } catch (const profile::Error& e) {
        throw Failure(e.what());
    }
    // Check the pack's patchsets parse, so a broken one fails here and not in the game. Whether
    // they match this game build is the agent's job (it has the loaded exe).
    for (const std::wstring& file : profile::patchset_files(profile_dir)) {
        std::string text;
        if (unsigned long err = read_file(file, text)) throw Failure(utf8(file) + ": " + win_error(err));
        try {
            patch::Patchset::parse(text, utf8(file));
        } catch (const patch::Error& e) {
            throw Failure(e.what());
        }
    }

    // The game reads its master server from data\address.cfg; the agent redirects that to the
    // profile's copy, written here.
    std::optional<std::string> cfg;
    try {
        cfg = profile::address_cfg(prof);
    } catch (const profile::Error& e) {
        throw Failure(e.what());
    }
    if (cfg) {
        const std::wstring dir = profile_dir + L"\\" + profile::kFilesDir + L"\\data";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) throw Failure(utf8(dir) + ": " + ec.message());
        if (unsigned long err = write_file(dir + L"\\address.cfg", *cfg))
            throw Failure("address.cfg: " + win_error(err));
    }

    const std::wstring dll_arg = args.dll ? *args.dll : exe_dir() + L"\\sc_agent.dll";
    if (!is_file(dll_arg)) throw Failure("agent not found at " + utf8(dll_arg));
    const std::wstring dll = profile::normalize(dll_arg);

    const std::wstring game = profile::normalize(args.game);
    size_t slash = game.rfind(L'\\');
    if (slash == std::wstring::npos) throw Failure("bad --game path");
    std::wstring game_dir = game.substr(0, slash);
    if (game_dir.size() == 2 && game_dir[1] == L':') game_dir += L'\\';  // C:\game.exe runs in C:\, not C:

    std::vector<std::wstring> game_args;
    if (args.game_args) game_args = *args.game_args;
    else
        for (const std::string& a : profile::game_args(prof)) game_args.push_back(wide(a));
    std::wstring cmdline = quote(game);
    for (const std::wstring& a : game_args) cmdline += L" " + quote(a);

    // The game inherits our environment; the agent finds the profile through it.
    SetEnvironmentVariableW(profile::kProfileEnv, profile_dir.c_str());

    say("profile " + utf8(profile_dir));
    say("starting " + utf8(cmdline));

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    // CreateProcessW may write to the command line buffer, so it gets a copy it owns.
    std::wstring cmd = cmdline;
    if (!CreateProcessW(game.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        game_dir.c_str(), &si, &pi))
        throw Failure(last_error("CreateProcessW"));

    // Everything the game starts (the bundled local server, for one) joins this job, so it can be
    // stopped with the game; otherwise leftovers keep the Proton session, and the launcher, waiting.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job || !AssignProcessToJobObject(job, pi.hProcess))
        say(last_error("job object setup") + "; processes the game starts will outlive it");

    try {
        inject(pi.hProcess, dll);
    } catch (...) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        throw;
    }
    say("agent injected, resuming game");

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    // Stay alive until the game exits so Proton keeps the session open.
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    say("game exited with " + std::to_string(code));
    if (job) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
        if (QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof info, nullptr) &&
            info.ActiveProcesses > 0)
            say("stopping " + std::to_string(info.ActiveProcesses) + " process(es) the game left running");
        TerminateJobObject(job, 0);
        CloseHandle(job);
    }
    return static_cast<int>(code);
}

}  // namespace

std::wstring quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    // CommandLineToArgvW's rules: backslashes are literal unless they come before a quote; there,
    // 2n backslashes + quote = n backslashes and the quote ends the argument, 2n+1 = n backslashes
    // and a literal quote. So double the backslashes before a quote (and before the closing one).
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
        } else {
            out.append(slashes, L'\\');
        }
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

int run(const std::vector<std::wstring>& args) {
    try {
        return launch(args);
    } catch (const std::exception& e) {
        say(std::string("error: ") + e.what());
        return 1;
    }
}

}  // namespace sf::inject
