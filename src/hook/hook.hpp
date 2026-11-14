#pragma once

// Thin wrapper over MinHook: redirect a function in another DLL to ours. Used by the agent
// (game.exe, x64) and the server proxy (x86).
//
// After install(target, detour, &original), every call to `target` runs `detour` instead, and
// `original` points at a trampoline that runs the real function. MinHook writes `original`
// before the hook goes live, so a detour can always call through it.
//
// For a set of hooks that must go live together, create() each one, then enable_all().

#include <string>

namespace sf::hook {

// Each returns "" on success, else what went wrong (for the log).
std::string create(void* target, void* detour, void** original);  // prepared, not live yet
std::string enable_all();                                         // every created hook goes live
std::string install(void* target, void* detour, void** original);  // create + enable this one

// Typed versions, so callers don't cast: install(real_fn, my_fn, &g_original_fn).
template <class Fn>
std::string create(Fn target, Fn detour, Fn* original) {
    return create(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original));
}
template <class Fn>
std::string install(Fn target, Fn detour, Fn* original) {
    return install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
                   reinterpret_cast<void**>(original));
}

}  // namespace sf::hook
