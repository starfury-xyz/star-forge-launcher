// A plugin for load_test's plugin scenarios, written against the public plugin_api.h like a
// pack author's would be. It logs a line naming the profile, and fails (returns 7) if the
// profile has a file called plugin_fail.

#include <windows.h>

#include <string>

#include "agent/plugin_api.h"

extern "C" __declspec(dllexport) int starforge_plugin_init(const StarForgePluginApi* api) {
    if (!api || api->version < 1) return 1;
    const std::wstring profile = api->profile_dir;
    const std::string msg = "hello from test plugin, profile has " + std::to_string(profile.size()) + " chars";
    api->log(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    if (GetFileAttributesW((profile + L"\\plugin_fail").c_str()) != INVALID_FILE_ATTRIBUTES) return 7;
    return 0;
}
