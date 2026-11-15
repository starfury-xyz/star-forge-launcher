// Steam Deck Game Mode (a gamescope session): gamescope shows only windows whose STEAM_GAME X
// property is the app Steam started. Steam's overlay library normally sets it on the game's
// windows, but umu clears LD_PRELOAD in gamescope sessions, so the game's windows stay untagged
// and gamescope keeps showing the launcher. WindowTagger sets the property on them instead (what
// umu itself does only when it runs as a Flatpak).
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

namespace sf::launch::gamescope {

// The app id gamescope expects on the game's windows when the launcher runs in a gamescope
// session started by Steam, else nullopt. `desktop`/`session_desktop`/`steam_game_id` are
// XDG_CURRENT_DESKTOP, XDG_SESSION_DESKTOP and SteamGameId: a Steam game's id as is, a non-Steam
// shortcut's 64-bit id shifted down to its app id (its top 32 bits).
std::optional<std::uint32_t> app_id(const char* desktop, const char* session_desktop, const char* steam_game_id);
std::optional<std::uint32_t> app_id();  // from the environment

// Until destroyed, tags new top-level Wine windows (class steam_app_* or *.exe) without a
// STEAM_GAME on $DISPLAY with `app`. Does nothing when libX11 or the display is unavailable.
class WindowTagger {
public:
    explicit WindowTagger(std::uint32_t app);
    ~WindowTagger();
    WindowTagger(const WindowTagger&) = delete;
    WindowTagger& operator=(const WindowTagger&) = delete;

private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// Whether X window class `cls` is a Wine window's (Proton: steam_app_<id>; Wine: the exe's name).
bool is_wine_class(const std::string& cls);

}  // namespace sf::launch::gamescope
