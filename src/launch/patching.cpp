#include "launch/patching.hpp"

#include <algorithm>
#include <stdexcept>

#include "patch/core.hpp"

namespace sf::launch::patching {
namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

const patch::Choices* choices(const pack::Profile& profile, const std::string& exe) {
    for (const auto& [module, c] : profile.patch_choices)
        if (lower(module) == lower(exe)) return &c;
    return nullptr;
}

// The module file a patchset targets: next to game.exe, or under the game root.
std::optional<fs::path> module_file(const fs::path& win64, const std::string& exe) {
    auto find = [&](const fs::path& dir) -> std::optional<fs::path> {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (lower(ustr(it->path().filename())) == lower(exe)) return it->path();
        return std::nullopt;
    };
    if (auto f = find(win64)) return f;
    if (win64.has_parent_path()) return find(win64.parent_path());
    return std::nullopt;
}

}  // namespace

std::vector<Loaded> all_sets(const fs::path& profile_dir, const pack::Profile& profile) {
    std::vector<Loaded> out;
    try {
        for (auto& set : patch::core::sets(patch::core::requested(profile.patches.core, profile.patch_choices))) out.push_back({std::move(set), ""});
    } catch (const std::exception& e) {
        out.push_back({std::nullopt, e.what()});
    }
    for (auto& l : load_sets(profile_dir)) out.push_back(std::move(l));
    return out;
}

std::vector<Loaded> load_sets(const fs::path& profile_dir) {
    std::vector<Loaded> out;
    for (const fs::path& file : pack::patchset_files(profile_dir)) {
        std::error_code ec;
        auto text = read_text(file, &ec);
        if (!text) {
            out.push_back({std::nullopt, io_error(file, ec)});
            continue;
        }
        try {
            out.push_back({patch::Patchset::parse(*text, std::string(pack::kPatchesDir) + "/" + ustr(file.filename())), ""});
        } catch (const std::exception& e) {
            out.push_back({std::nullopt, e.what()});
        }
    }
    return out;
}

Summary summary(const fs::path& profile_dir, const pack::Profile& profile) {
    Summary s;
    s.plugins = profile.patches.plugins.size();
    for (auto& l : all_sets(profile_dir, profile)) {
        if (!l.set) {
            s.errors.push_back(l.error);
            continue;
        }
        s.patches += l.set->enabled(choices(profile, l.set->exe)).on.size();
        if (std::find(s.modules.begin(), s.modules.end(), l.set->exe) == s.modules.end()) s.modules.push_back(l.set->exe);
    }
    return s;
}

std::vector<std::string> preflight(const fs::path& profile_dir, const pack::Profile& profile, const fs::path& game_exe) {
    const fs::path win64 = game_exe.has_parent_path() ? game_exe.parent_path() : fs::path(".");
    std::vector<std::string> problems, warnings;
    const std::vector<Loaded> sets = all_sets(profile_dir, profile);
    std::vector<const patch::Patchset*> loaded;
    for (const auto& l : sets)
        if (l.set) loaded.push_back(&*l.set);
    warnings = patch::core::unknown_choices(loaded, profile.patch_choices);
    for (const auto& l : sets) {
        if (!l.set) {
            problems.push_back(l.error);
            continue;
        }
        const patch::Patchset& set = *l.set;
        auto file = module_file(win64, set.exe);
        if (!file) {
            problems.push_back(set.exe + " is not in the game folder");
            continue;
        }
        std::error_code ec;
        auto data = read_text(*file, &ec);
        if (!data) {
            problems.push_back(io_error(*file, ec));
            continue;
        }
        patch::Report r = patch::verify_file(set, patch::Bytes(data->begin(), data->end()), choices(profile, set.exe));
        problems.insert(problems.end(), r.errors.begin(), r.errors.end());
        // (r.warnings are unknown switches seen from this set alone: unknown_choices covers them.)
        for (const auto& s : r.skipped) warnings.push_back(set.exe + ": optional patch skipped: " + s);
    }
    for (const std::string& plugin : profile.patches.plugins) {
        auto rel = pack::patches_rel(plugin);
        std::error_code ec;
        if (!rel || !fs::is_regular_file(profile_dir / pack::kPatchesDir / upath(*rel), ec))
            problems.push_back("plugin " + plugin + " is missing from the profile");
    }
    if (problems.empty()) return warnings;
    std::string text = "The pack's game patches don't match this game: ";
    for (size_t i = 0; i < problems.size(); ++i) text += (i ? "; " : "") + problems[i];
    if (profile.patches.on_error == pack::OnError::Exit) throw std::runtime_error(text);
    warnings.insert(warnings.begin(), text + ". Starting anyway (the pack allows it).");
    return warnings;
}

}  // namespace sf::launch::patching
