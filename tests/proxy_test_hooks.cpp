// Midhooks for proxy_test's midhook scenarios, compiled into the test build's proxy DLL. The
// sites are hook_site_* in proxy_test.cpp: `xor ecx, ecx; mov eax, 1 (the 5 stolen bytes);
// add eax, ecx; ret`, so they return 1 unhooked.
#include "proxy/sfp_hook.hpp"

#include <windows.h>

// Continues: ecx = 41, then the stolen mov eax, 1 runs, so the site returns 42.
SFP_MIDHOOK(sf_test_add) {
    r->ecx = 41;
    sfp::log("sf_test_add ran");
    return 0;
}

// Jumps instead: to proxy_test's jump_target (which returns 99), skipping the stolen bytes.
SFP_MIDHOOK(sf_test_jump) {
    return uint32_t(uintptr_t(GetProcAddress(GetModuleHandleW(nullptr), "jump_target")));
}
