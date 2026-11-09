#pragma once

// The backend's state, shared by its .cpp files.

#include <unordered_map>

#include "gui/backend.hpp"

namespace sf::gui {

// Detection and launch state behind the play button.
struct Backend::Game {
    mutable std::mutex mu;
    uint64_t revision = 0;
    std::optional<Phase> phase;
    std::optional<std::string> reason;
    std::optional<launch::SteamFix> fix;
    bool fix_opened = false;  // Steam was opened for `fix`; the play button is Retry again
    std::optional<launch::Setup> setup;
};

// Packs unpacked for review, by token, until installed or discarded.
struct Backend::Reviews {
    std::mutex mu;
    std::map<uint64_t, launch::packs::Staged> staged;
    uint64_t next = 0;
    // Whether the interface listens for links yet, and a link that arrived before it did.
    bool listening = false;
    std::optional<std::string> pending;
};

// Downloads in progress, by id, with their cancel flags.
struct Backend::DownloadState {
    std::atomic<uint64_t> next{0};
    std::mutex mu;
    std::unordered_map<uint64_t, std::shared_ptr<std::atomic<bool>>> running;
};

// The launcher update found by the last check, kept for install_launcher_update.
struct Backend::Updates {
    std::mutex mu;
    std::optional<updater::Release> found;
};

// Tauri's argument checks: a missing or mistyped argument fails the command.
std::string arg_str(const char* command, const json& args, const char* key);
std::optional<std::string> arg_opt_str(const char* command, const json& args, const char* key);
uint64_t arg_u64(const char* command, const json& args, const char* key);
bool arg_bool(const char* command, const json& args, const char* key);
const json& arg(const char* command, const json& args, const char* key);

// The profiles folder. Throws std::runtime_error.
fs::path profiles_dir();
// An existing profile's folder. Throws std::runtime_error.
fs::path profile_dir(const std::string& id);

}  // namespace sf::gui
