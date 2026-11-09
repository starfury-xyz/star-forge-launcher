// Started by process_test: prints how it was started, one "key=value" per line:
//   cwd=<working folder>, arg=<each argument>, env:<NAME>=<value or (unset)> for each name in
//   SF_SHOW (comma-separated). Exits with the code in SF_EXIT (default 0).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>
#endif

namespace {

std::string env(const std::string& name, bool& set) {
#ifdef _WIN32
    std::wstring w = std::filesystem::u8path(name).wstring();
    DWORD n = GetEnvironmentVariableW(w.c_str(), nullptr, 0);
    set = n != 0;
    if (!set) return "";
    std::wstring v(n, L'\0');
    v.resize(GetEnvironmentVariableW(w.c_str(), v.data(), n));
    return std::filesystem::path(v).u8string();
#else
    const char* v = std::getenv(name.c_str());
    set = v != nullptr;
    return v ? v : "";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);  // "\n" stays "\n" (text mode would write "\r\n")
    // The UTF-16 command line, so non-ASCII arguments survive.
    (void)argc;
    (void)argv;
    int n = 0;
    wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; ++i) args.push_back(std::filesystem::path(w[i]).u8string());
    LocalFree(w);
#else
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
    std::printf("cwd=%s\n", std::filesystem::current_path().u8string().c_str());
    for (const auto& a : args) std::printf("arg=%s\n", a.c_str());
    bool set = false;
    std::string show = env("SF_SHOW", set);
    size_t start = 0;
    while (set && start < show.size()) {
        size_t end = show.find(',', start);
        if (end == std::string::npos) end = show.size();
        std::string name = show.substr(start, end - start);
        bool has = false;
        std::string value = env(name, has);
        std::printf("env:%s=%s\n", name.c_str(), has ? value.c_str() : "(unset)");
        start = end + 1;
    }
    std::string code = env("SF_EXIT", set);
    return set ? std::atoi(code.c_str()) : 0;
}
