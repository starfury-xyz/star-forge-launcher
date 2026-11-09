// A DLL that load_test loads *after* the agent, to test patchsets for DLLs that aren't loaded
// yet: the agent must patch `marker` when the loader maps the DLL, before its DllMain runs.
//
// `marker` sits alone in its own section, .sfmark, so the test can find its address in the DLL
// file (section table) before the DLL is loaded, to write the patchset.

#include <windows.h>

#pragma section(".sfmark", read, write)

extern "C" {
// What the patchset changes. volatile: the compiler must really read it, not assume its value.
__declspec(allocate(".sfmark")) __declspec(dllexport) volatile unsigned char marker[8] = {'D', 'L', 'L', '-', 'O', 'R', 'I', 'G'};
// What marker held when DllMain ran.
__declspec(dllexport) unsigned char seen_in_dllmain[8] = {};
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH)
        for (int i = 0; i < 8; ++i) seen_in_dllmain[i] = marker[i];
    return TRUE;
}
