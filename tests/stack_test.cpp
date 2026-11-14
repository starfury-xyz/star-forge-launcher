// A stand-in server exe for the [stack] scenarios of proxy_test: it imports the proxy (so the
// proxy loads before its entry point runs, like libcrypto in a real server exe), then prints the
// main thread's stack size. `stack_test deep` also uses ~6 MB of stack first, which the stock
// 1 MB would not survive.

#include <windows.h>

#include <cstdio>
#include <cstring>

extern "C" __declspec(dllimport) const char* __cdecl sfp_version();

namespace {

// Recursion with a 64 KB frame; volatile so the compiler keeps every frame.
int deep(int n) {
    volatile char frame[64 * 1024];
    std::memset(const_cast<char*>(frame), n, sizeof frame);
    return n == 0 ? frame[0] : deep(n - 1) + frame[1];
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("proxy %s\n", sfp_version());
    if (argc > 1 && std::strcmp(argv[1], "deep") == 0) std::printf("deep ok %d\n", deep(96));  // 96 x 64 KB = 6 MB
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    std::printf("stack %lu\n", static_cast<unsigned long>(high - low));
    return 7;  // proxy_test checks the exit code comes through
}
