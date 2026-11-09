#include "agent/hook.hpp"

#include <windows.h>

#include <MinHook.h>

namespace sf::agent::hook {

std::string install(void* target, void* detour, void** original) {
    // MinHook needs one MH_Initialize per process before any hook. The Rust crate does this
    // lazily too. Agent setup runs on the loader thread only, so a plain flag is enough.
    static MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return std::string("MH_Initialize: ") + MH_StatusToString(init);
    if (MH_STATUS s = MH_CreateHook(target, detour, original); s != MH_OK)
        return std::string("MH_CreateHook: ") + MH_StatusToString(s);
    if (MH_STATUS s = MH_EnableHook(target); s != MH_OK) return std::string("MH_EnableHook: ") + MH_StatusToString(s);
    return {};
}

}  // namespace sf::agent::hook
