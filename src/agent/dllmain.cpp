// Agent DLL injected into game.exe by sc-inject (started from sc-launch). C++ port of
// star-forge crates/sc-agent; see agent.cpp for what it does on load.

#include <windows.h>

#include "agent/agent.hpp"

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // Never let an exception unwind into the loader. Returning FALSE makes LoadLibrary
        // fail, and sc-inject then stops the game.
        try {
            return sf::agent::init() ? TRUE : FALSE;
        } catch (...) {
            return TRUE;
        }
    }
    return TRUE;
}
