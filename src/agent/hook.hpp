#pragma once

// Thin wrapper over MinHook: redirect a function in another DLL to ours.
//
// After install(target, detour, &original), every call to `target` runs `detour` instead, and
// `original` points at a trampoline that runs the real function. MinHook writes `original`
// before the hook goes live, so a detour can always call through it.

#include <string>

namespace sf::agent::hook {

// Returns "" on success, else what went wrong (for the log).
std::string install(void* target, void* detour, void** original);

// Typed version, so callers don't cast: install(real_fn, my_fn, &g_original_fn).
template <class Fn>
std::string install(Fn target, Fn detour, Fn* original) {
    return install(reinterpret_cast<void*>(target), reinterpret_cast<void*>(detour),
                   reinterpret_cast<void**>(original));
}

}  // namespace sf::agent::hook
