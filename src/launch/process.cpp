#include "launch/process.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
extern char** environ;
#endif

namespace sf::launch {

std::optional<std::string> Command::env_value(const std::string& key) const {
    std::optional<std::string> v = get_env(key.c_str());
    for (const auto& [k, value] : env)
        if (k == key) v = value;
    return v;
}

std::string quote_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos) return arg;
    // See sc-inject's quote() for the rules.
    std::string out = "\"";
    size_t slashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        out.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, '\\');
    out += '"';
    return out;
}

Process::Process(Process&& o) noexcept { *this = std::move(o); }

#ifdef _WIN32

Process& Process::operator=(Process&& o) noexcept {
    std::swap(handle_, o.handle_);
    return *this;
}

Process::~Process() {
    if (handle_) CloseHandle(handle_);
}

std::optional<int> Process::wait() {
    if (!handle_) return std::nullopt;
    WaitForSingleObject(handle_, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(handle_, &code);
    return static_cast<int>(code);
}

namespace {

std::wstring wide(const std::string& s) { return upath(s).wstring(); }

// This process's environment with the command's changes, as a CreateProcessW block: sorted by
// name ignoring case (as Windows expects), each "NAME=value\0", then a final \0.
std::wstring env_block(const Command& cmd) {
    auto less = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    };
    std::map<std::wstring, std::wstring, decltype(less)> vars(less);
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (const wchar_t* e = block; *e; e += wcslen(e) + 1) {
            std::wstring kv = e;
            size_t eq = kv.find(L'=', 1);  // "=C:=C:\..." entries start with '='
            if (eq != std::wstring::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& [k, v] : cmd.env) {
        if (v) vars[wide(k)] = wide(*v);
        else vars.erase(wide(k));
    }
    std::wstring out;
    for (const auto& [k, v] : vars) out += k + L"=" + v + L'\0';
    out += L'\0';
    return out;
}

[[noreturn]] void fail(const Command& cmd, DWORD err) {
    char msg[512] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, msg, sizeof msg, nullptr);
    std::string why = msg;
    while (!why.empty() && (why.back() == '\n' || why.back() == '\r' || why.back() == '.')) why.pop_back();
    throw std::runtime_error("could not start " + ustr(cmd.program) + ": " + why + " (error " + std::to_string(err) + ")");
}

}  // namespace

Process spawn(const Command& cmd, const Output& out, bool no_window) {
    // No application name: CreateProcessW then finds a bare program name on PATH, like Rust.
    std::wstring line = wide(quote_arg(ustr(cmd.program)));
    for (const auto& a : cmd.args) line += L" " + wide(quote_arg(a));
    std::wstring env = env_block(cmd);

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE std_out = INVALID_HANDLE_VALUE;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (out.kind != Output::Kind::Inherit) {
        std_out = CreateFileW(out.kind == Output::Kind::File ? out.file.wstring().c_str() : L"NUL", GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (std_out == INVALID_HANDLE_VALUE) fail(cmd, GetLastError());
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nullptr;
        si.hStdOutput = si.hStdError = std_out;
    }
    PROCESS_INFORMATION pi{};
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (no_window ? CREATE_NO_WINDOW : 0);
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, std_out != INVALID_HANDLE_VALUE, flags, env.data(),
                             cmd.cwd ? cmd.cwd->wstring().c_str() : nullptr, &si, &pi);
    DWORD err = GetLastError();
    if (std_out != INVALID_HANDLE_VALUE) CloseHandle(std_out);
    if (!ok) fail(cmd, err);
    CloseHandle(pi.hThread);
    Process p;
    p.handle_ = pi.hProcess;
    return p;
}

fs::path current_exe() {
    std::wstring buf(32768, L'\0');
    buf.resize(GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size())));
    return fs::path(buf);
}

#else  // POSIX

Process& Process::operator=(Process&& o) noexcept {
    std::swap(pid_, o.pid_);
    return *this;
}

Process::~Process() = default;  // like Rust's Child: dropping it doesn't wait or kill

std::optional<int> Process::wait() {
    if (pid_ < 0) return std::nullopt;
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
    }
    pid_ = -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return std::nullopt;
}

namespace {

// `name` on PATH (as the child's PATH would find it), or `name` itself if it has a '/'.
std::string find_program(const Command& cmd) {
    std::string name = ustr(cmd.program);
    if (name.find('/') != std::string::npos) return name;
    std::string path = cmd.env_value("PATH").value_or("/usr/local/bin:/usr/bin:/bin");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string dir = path.substr(start, end - start);
        std::string full = (dir.empty() ? "." : dir) + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return full;
        start = end + 1;
    }
    return name;
}

}  // namespace

Process spawn(const Command& cmd, const Output& out, bool) {
    // Everything the child needs is prepared before fork(): after it, only async-signal-safe
    // calls (open, dup2, chdir, execve, _exit).
    const std::string program = find_program(cmd);
    std::vector<std::string> argv_s{ustr(cmd.program)};
    argv_s.insert(argv_s.end(), cmd.args.begin(), cmd.args.end());
    std::vector<char*> argv;
    for (auto& a : argv_s) argv.push_back(a.data());
    argv.push_back(nullptr);

    std::map<std::string, std::string> vars;
    for (char** e = environ; *e; ++e) {
        std::string kv = *e;
        size_t eq = kv.find('=');
        if (eq != std::string::npos) vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    for (const auto& [k, v] : cmd.env) {
        if (v) vars[k] = *v;
        else vars.erase(k);
    }
    std::vector<std::string> env_s;
    for (const auto& [k, v] : vars) env_s.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& e : env_s) envp.push_back(e.data());
    envp.push_back(nullptr);

    const std::string cwd = cmd.cwd ? ustr(*cmd.cwd) : "";
    const std::string out_file = out.kind == Output::Kind::File ? ustr(out.file) : "/dev/null";

    // The child reports a failed chdir/exec through this pipe (closed on a successful exec).
    // The highest descriptor the child may have inherited, for closing them when close_range is
    // not available.
    rlimit lim{};
    const int max_fd = getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur != RLIM_INFINITY
                           ? static_cast<int>(std::min<rlim_t>(lim.rlim_cur, 65536))
                           : 65536;

    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0) throw std::runtime_error("could not start " + program + ": pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(report[0]);
        close(report[1]);
        throw std::runtime_error("could not start " + program + ": " + std::strerror(errno));
    }
    if (pid == 0) {
        close(report[0]);
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) dup2(in, 0);
        if (out.kind != Output::Kind::Inherit) {
            int fd = open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, 1);
                dup2(fd, 2);
            }
        }
        // Start the child clean: only stdin/stdout/stderr (and the report pipe, closed on exec),
        // nothing blocked, SIGPIPE back to default. Started from Steam in Game Mode, the launcher
        // holds dozens of Steam/WebKit descriptors without close-on-exec; passing them on left
        // SteamLinuxRuntime_sniper/run waiting forever on its own $(readlink) pipe.
        auto close_from = [max_fd](int from, int to) {
#ifdef __linux__
            if (from > to || syscall(SYS_close_range, from, to, 0) == 0) return;
#endif
            for (int f = from; f <= to && f < max_fd; ++f) close(f);
        };
        close_from(3, report[1] - 1);
        close_from(report[1] + 1, INT_MAX);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        signal(SIGPIPE, SIG_DFL);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            int e = errno;
            (void)!write(report[1], &e, sizeof e);
            _exit(127);
        }
        execve(program.c_str(), argv.data(), envp.data());
        int e = errno;
        (void)!write(report[1], &e, sizeof e);
        _exit(127);
    }
    close(report[1]);
    int child_errno = 0;
    ssize_t n;
    while ((n = read(report[0], &child_errno, sizeof child_errno)) < 0 && errno == EINTR) {
    }
    close(report[0]);
    if (n > 0) {
        int status;
        waitpid(pid, &status, 0);
        throw std::runtime_error("could not start " + program + ": " + std::strerror(child_errno));
    }
    Process p;
    p.pid_ = pid;
    return p;
}

fs::path current_exe() {
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
}

#endif

}  // namespace sf::launch
