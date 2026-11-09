/* STAR FORGE plugin interface. Plain C, so a plugin can be written in C, C++ or anything that
 * can export a C function. Include this header in your plugin.
 *
 * A pack lists its plugins in pack.json ("patches": {"plugins": ["patches/my_plugin.dll"]}).
 * After applying the pack's patchsets, the agent loads each plugin DLL into game.exe (the game is
 * still suspended, so this runs before any game code). A plugin may export
 *
 *     int starforge_plugin_init(const StarForgePluginApi* api);
 *
 * Return 0 for success. Anything else is a failure, handled like a patch that doesn't apply
 * (the pack's patches.on_error: "exit" stops the game, "continue" carries on).
 *
 * Same layout as the Rust agent's PluginApi, so plugins work with either agent. */

#ifndef STARFORGE_PLUGIN_API_H
#define STARFORGE_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Append-only: new fields go at the end, and `version` goes up when they're added. */
typedef struct StarForgePluginApi {
    uint32_t version; /* 1 */
    /* Profile folder, NUL-terminated UTF-16. Valid only during starforge_plugin_init: copy it. */
    const wchar_t* profile_dir;
    /* Writes one UTF-8 line (`len` bytes, no NUL needed) to agent.log, as "plugin: <text>". */
    void (*log)(const uint8_t* text, size_t len);
} StarForgePluginApi;

typedef int (*StarForgePluginInit)(const StarForgePluginApi* api);

#ifdef __cplusplus
}
#endif

#endif /* STARFORGE_PLUGIN_API_H */
