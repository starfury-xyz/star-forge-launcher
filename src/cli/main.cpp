// sc-launch: the command-line launcher (port of crates/sc-launch/src/main.rs). Portable: on Linux
// it starts the game through Proton, on Windows directly. A thin layer over sf_launch.

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "launch/env.hpp"
#include "launch/gamescope.hpp"
#include "launch/launch.hpp"
#include "launch/mods.hpp"
#include "launch/packs.hpp"
#include "launch/patching.hpp"
#include "launch/prepare.hpp"
#include "launch/process.hpp"
#include "launch/profiles.hpp"
#include "launch/remote.hpp"
#include "launch/servers.hpp"
#include "launch/signing.hpp"
#include "pack/serde.hpp"

using namespace sf::launch;
namespace pk = sf::pack;

namespace {

const char* kUsage =
    "sc-launch - Star Conflict launcher with switchable profiles\n"
    "\n"
    "USAGE:\n"
    "  sc-launch [OPTIONS] list              List profiles\n"
    "  sc-launch [OPTIONS] info              Show the detected game, tools and Proton setup\n"
    "  sc-launch [OPTIONS] run <profile> [-- <game args>]\n"
    "                                        Start the game with a profile (game args\n"
    "                                        replace the profile's own)\n"
    "  sc-launch [OPTIONS] install <zip|folder|url> [--yes]\n"
    "                                        Install a server pack, or update the\n"
    "                                        profile that already has it\n"
    "  sc-launch [OPTIONS] updates           Check installed packs for new versions\n"
    "  sc-launch [OPTIONS] update <profile> [--yes]\n"
    "                                        Install the newest version of a profile's pack\n"
    "  sc-launch [OPTIONS] patches <profile> List a profile's game patches and which are on\n"
    "  sc-launch [OPTIONS] patches <profile> on|off|default <exe> <id>\n"
    "                                        Switch one of them on or off, or back to what\n"
    "                                        the pack says (saved in the profile's local.json)\n"
    "\n"
    "FOR SERVER HOSTS:\n"
    "  sc-launch server status <cloud-dir>   Check a server install the way its processes\n"
    "                                        will at startup: proxy DLL, exe builds, patches\n"
    "  sc-launch [OPTIONS] server install <cloud-dir> [<proxy dll>]\n"
    "                                        Put the server proxy in (the game's DLL is\n"
    "                                        kept); run again after a Steam verify/update\n"
    "  sc-launch server uninstall <cloud-dir>\n"
    "                                        Put the game's own DLL back\n"
    "  sc-launch [OPTIONS] server start <cloud-dir>\n"
    "                                        Start the server: MongoDB, then Hub and its\n"
    "                                        services (on Linux in the game's Proton prefix)\n"
    "  sc-launch [OPTIONS] server stop <cloud-dir>\n"
    "                                        Stop it (Ctrl+C to Hub, then MongoDB)\n"
    "  sc-launch [OPTIONS] server logs <cloud-dir> [<log>]\n"
    "                                        List its logs (the game's newest per\n"
    "                                        service, and STAR FORGE's), or show the end\n"
    "                                        of one, by name or label\n"
    "  sc-launch server on|off|default <cloud-dir> <exe> <id>\n"
    "                                        Switch a server patch (saved in\n"
    "                                        starforge/patcher.toml; applies on restart)\n"
    "\n"
    "FOR SERVER TEAMS:\n"
    "  sc-launch pack check <zip|folder|url> Show what a pack would do and check it the\n"
    "                                        way the launcher does, without installing\n"
    "  sc-launch pack keygen <key file>      Make a pack signing key (keep it secret)\n"
    "  sc-launch pack files <folder> <list> [--base-url <url>] [--key <key file>]\n"
    "                                        Write a file list for the game files in\n"
    "                                        <folder> (laid out like the game folder):\n"
    "                                        <list> ending in .txt is SCMOD1 (needs\n"
    "                                        --base-url), otherwise STAR FORGE JSON. With\n"
    "                                        a key, also writes <list>.sig\n"
    "  sc-launch pack sign <folder> --key <key file>\n"
    "                                        Write manifest.json and manifest.sig for the\n"
    "                                        pack in <folder>; the key can also come from\n"
    "                                        STAR_FORGE_PACK_KEY (the key file's text)\n"
    "\n"
    "OPTIONS:\n"
    "  --profiles-dir <dir>   Profile folder (default: the per-user data folder)\n"
    "  --game-dir <dir>       Star Conflict install (default: found through Steam)\n"
    "  --tools-dir <dir>      Folder with sc-inject.exe and sc_agent.dll (default: next to sc-launch)\n"
    "  --runner <name>        umu (the Linux default: umu-launcher with Proton), steam-proton (Steam's\n"
    "                         own setup; deprecated) or a Wine-style command such as wine or a wrapper\n"
    "                         script, run as <cmd> program.exe args (the macOS default is wine)\n"
    "  --proton <dir>         Proton build (default: umu's own UMU-Proton; steam-proton: the one Steam\n"
    "                         last ran the game with)\n"
    "  --wine-prefix <dir>    prefix for umu and Wine-style runners (default: <data dir>/prefixes/<umu|wine>)\n"
    "  --yes                  Install without asking for confirmation\n"
    "  --key <file>           Pack signing key, for pack sign and pack files\n"
    "  --base-url <url>       Where the files of a pack files list will be hosted\n"
    "\n"
    "ENVIRONMENT (used when the matching option is not given):\n"
    "  STAR_FORGE_GAME_DIR, STAR_FORGE_TOOLS_DIR, STAR_FORGE_PROTON, STAR_FORGE_RUNNER,\n"
    "  STAR_FORGE_WINEPREFIX, STAR_FORGE_DATA_DIR\n"
    "  Development builds also read them from the repo's .env file.\n";

struct Cli {
    Options opts;
    std::optional<fs::path> profiles_dir;
    std::vector<std::string> command;
    std::vector<std::string> game_args;
    bool yes = false;
    std::optional<fs::path> key;
    std::optional<std::string> base_url;
};

Cli parse(const std::vector<std::string>& args) {
    Cli cli;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error(a + " needs a value");
            return args[++i];
        };
        if (a == "--profiles-dir") cli.profiles_dir = upath(value());
        else if (a == "--game-dir") cli.opts.game_dir = upath(value());
        else if (a == "--tools-dir") cli.opts.tools_dir = upath(value());
        else if (a == "--proton") cli.opts.proton = upath(value());
        else if (a == "--runner") cli.opts.runner = value();
        else if (a == "--wine-prefix") cli.opts.wine_prefix = upath(value());
        else if (a == "--key") cli.key = upath(value());
        else if (a == "--base-url") cli.base_url = value();
        else if (a == "-h" || a == "--help") cli.command = {"help"};
        else if (a == "--yes" || a == "-y") cli.yes = true;
        else if (a == "--") {
            cli.game_args.assign(args.begin() + static_cast<long>(i) + 1, args.end());
            break;
        } else if (a.rfind("--", 0) == 0) throw std::runtime_error("unknown option " + a);
        else cli.command.push_back(a);
    }
    return cli;
}

// Rust's {:<n}: padded with spaces to n characters (not bytes).
std::string pad(const std::string& s, size_t n) {
    size_t chars = 0;
    for (unsigned char c : s) chars += (c & 0xc0) != 0x80;
    return chars >= n ? s : s + std::string(n - chars, ' ');
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

std::string describe_trust(const trust::Trust& t) {
    switch (t.kind) {
    case trust::Trust::Unsigned: return "not signed";
    case trust::Trust::NewKey: return "signed with a new key " + t.key->fingerprint();
    case trust::Trust::Pinned: return "signed by its publisher, key " + t.key->fingerprint();
    case trust::Trust::Known: return "signed by " + t.name + ", key " + t.key->fingerprint();
    }
    return "";
}

// The signing key from --key or STAR_FORGE_PACK_KEY.
std::optional<signing::SecretKey> signing_key(const Cli& cli, bool required) {
    std::string text;
    if (cli.key) {
        text = read_or_throw(*cli.key);
    } else if (auto env = get_env("STAR_FORGE_PACK_KEY")) {
        text = *env;
    } else {
        if (required) throw std::runtime_error("this needs --key <file> or STAR_FORGE_PACK_KEY");
        return std::nullopt;
    }
    return signing::SecretKey::parse(text);
}

// Writes a secret file readable only by its owner where that's possible; never over another.
void write_secret(const fs::path& path, const std::string& text) {
#ifdef _WIN32
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error(io_error(path, std::error_code(static_cast<int>(GetLastError()), std::system_category())));
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    const DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok) throw std::runtime_error(io_error(path, std::error_code(static_cast<int>(err), std::system_category())));
#else
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error(io_error(path, std::error_code(errno, std::generic_category())));
    const bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    const int err = errno;
    close(fd);
    if (!ok) throw std::runtime_error(io_error(path, std::error_code(err, std::generic_category())));
#endif
}

void print_review(const packs::Staged& staged, const std::optional<pk::Pack>& existing) {
    const pk::Pack& pack = staged.pack;
    std::printf("%s %s (%s)\n", pack.name.c_str(), pack.version.c_str(), pack.id.c_str());
    if (existing) std::printf("  updates the installed version %s\n", existing->version.c_str());
    if (!pack.authors.empty()) std::printf("  by %s\n", join(pack.authors, ", ").c_str());
    std::printf("  from %s%s\n", staged.origin.c_str(), staged.verified ? " (checksum verified)" : "");
    std::printf("  %s\n", describe_trust(staged.trust).c_str());
    std::printf("  server:  %s\n", pack.server.address ? pack.server.address->c_str() : "game default");
    if (pack.server.local) std::printf("  starts the local server with the game\n");
    if (staged.remote) {
        std::string size;
        if (auto b = staged.remote->bytes()) {
            char buf[64];
            std::snprintf(buf, sizeof buf, " (%.1f MB)", static_cast<double>(*b) / 1048576.0);
            size = buf;
        }
        std::printf("  replaces %zu game file(s), downloaded from %s%s\n", staged.files.size(), staged.remote->host().c_str(),
                    size.c_str());
    } else {
        std::printf("  replaces %zu game file(s)\n", staged.files.size());
    }
    for (size_t i = 0; i < staged.files.size() && i < 20; ++i) std::printf("    %s\n", staged.files[i].c_str());
    if (staged.files.size() > 20) std::printf("    ... and %zu more\n", staged.files.size() - 20);
    const patching::Summary patches = patching::summary(staged.root, pk::Profile::from(pack, pk::Local{}));
    if (!patches.modules.empty())
        std::printf("  applies %zu game patch(es) to %s\n", patches.patches, join(patches.modules, ", ").c_str());
    if (patches.plugins > 0) std::printf("  loads %zu native plugin(s) into the game\n", patches.plugins);
    if (!pack.game.args.empty()) std::printf("  game arguments: %s\n", join(pack.game.args, " ").c_str());
    for (const std::string& w : staged.warnings) std::printf("  note: %s\n", w.c_str());
}

std::optional<pk::Pack> existing_pack(const fs::path& profiles_dir, const std::string& id) {
    auto dir = profiles::find_pack(profiles_dir, id);
    if (!dir) return std::nullopt;
    try {
        return profiles::read_pack(*dir);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// Shows what a pack will do and installs it once the user agrees.
int install_reviewed(const packs::Staged& staged, const fs::path& profiles_dir, bool yes) {
    print_review(staged, existing_pack(profiles_dir, staged.pack.id));
    if (!yes) {
        std::printf("Install? [y/N] ");
        std::fflush(stdout);
        std::string answer;
        std::getline(std::cin, answer);
        const char* ws = " \t\r\n";
        size_t a = answer.find_first_not_of(ws);
        answer = a == std::string::npos ? "" : answer.substr(a, answer.find_last_not_of(ws) - a + 1);
        if (answer != "y" && answer != "Y" && answer != "yes") {
            packs::discard(staged);
            std::printf("not installed\n");
            return 1;
        }
    }
    const packs::Installed installed = packs::install(staged, profiles_dir, remote::quiet);
    std::printf("installed as profile %s\n", installed.name.c_str());
    if (installed.warning) std::fprintf(stderr, "warning: %s\n", installed.warning->c_str());
    return 0;
}

// One patch per line, grouped by module and patchset: "on"/"off", '*' when it's the player's own
// switch, then id and title, then anything worth knowing. `module` limits it to one module.
void print_toggles(const mods::List& list, const std::optional<std::string>& module = std::nullopt) {
    std::string group;
    for (const mods::Toggle& t : list.toggles) {
        if (module && t.module != *module) continue;
        const std::string g = t.module + " (" + t.source + (t.build.empty() ? "" : ", build " + t.build) + ")";
        if (g != group) std::printf("%s\n", (group = g).c_str());
        std::string notes;
        if (t.optional) notes += " [optional]";
        if (!t.conflicts.empty()) notes += " [clashes with " + join(t.conflicts, ", ") + "]";
        std::printf("  %s%s %s %s%s\n", t.on ? "on " : "off", t.chosen ? "*" : " ", pad(t.id, 28).c_str(), t.title.c_str(),
                    notes.c_str());
    }
}

// After a switch: warns if that patch is now on together with one it clashes with.
void warn_clashes(const mods::List& list, const std::string& module, const std::string& id) {
    std::fflush(stdout);  // after the line saying what was switched
    for (const std::string& c : mods::clashes_on(list, module, id))
        std::fprintf(stderr, "warning: %s clashes with %s, which is also on; switch one off or the patches won't apply\n",
                     id.c_str(), c.c_str());
}

// on|off|default as a switch value; throws for anything else.
std::optional<bool> switch_value(const std::string& word) {
    if (word == "on") return true;
    if (word == "off") return false;
    if (word == "default") return std::nullopt;
    throw std::runtime_error("expected on, off or default, not " + pk::serde::debug_str(word));
}

const char* describe_proxy(mods::ServerStatus::Proxy p) {
    switch (p) {
    case mods::ServerStatus::NoDll: return "MISSING: no libcrypto-1_1_tg.dll here; is this the game's cloud folder?";
    case mods::ServerStatus::Stock: return "not installed: the game's own libcrypto-1_1_tg.dll, so no patches apply";
    case mods::ServerStatus::Installed: return "installed (the original is kept as libcrypto-1_1_tg_orig.dll)";
    case mods::ServerStatus::Restored:
        return "NOT ACTIVE: libcrypto-1_1_tg.dll is the game's again (Steam \"verify files\" or an update?); install the proxy again";
    case mods::ServerStatus::Broken:
        return "BROKEN: the proxy without libcrypto-1_1_tg_orig.dll, or the other way round; verify the game files in Steam";
    }
    return "";
}

// sc-launch server status: what the server will do at startup. Returns 1 if it wouldn't start
// patched as configured.
int server_status(const fs::path& cloud) {
    const mods::ServerStatus st = mods::server_status(cloud);
    const mods::List list = mods::server(cloud);
    bool bad = st.proxy != mods::ServerStatus::Installed;
    std::printf("server:  %s\n", ustr(cloud).c_str());
    std::printf("proxy:   %s\n", describe_proxy(st.proxy));
    if (st.config)
        std::printf("config:  %s/%s (on_error = %s)\n", mods::kServerDir, mods::kServerConfig,
                    st.config->exit_on_error ? "exit" : "continue");
    for (const std::string& e : st.errors) std::printf("problem: %s\n", e.c_str());
    bad = bad || !st.errors.empty();
    if (st.exes.empty()) std::printf("no patchsets in %s/%s\n", mods::kServerDir, mods::kServerPatchsets);
    for (const mods::ExeCheck& c : st.exes) {
        std::printf("\n");
        print_toggles(list, c.module);
        if (c.report.errors.empty())
            std::printf("  check: OK, %zu patch(es) would apply to this %s (sha1 %s)\n", c.report.ok.size(),
                        c.module.c_str(), c.sha1->c_str());
        for (const auto& s : c.report.skipped) std::printf("  note: optional patch skipped: %s\n", s.c_str());
        for (const auto& w : c.report.warnings) std::printf("  warning: %s\n", w.c_str());
        for (const auto& e : c.report.errors) std::printf("  problem: %s\n", e.c_str());
        bad = bad || !c.report.errors.empty();
    }
    if (!list.toggles.empty()) std::printf("\n* = switched in patcher.toml; the rest follow each patch's default\n");
    std::printf("%s\n", bad ? "server has problems" : "server OK");
    return bad ? 1 : 0;
}

void check_profile_name(const std::string& name) {
    if (!profiles::valid_name(name)) throw std::runtime_error(pk::serde::debug_str(name) + " is not a valid profile name");
}

int run(const std::vector<std::string>& args) {
    const Cli cli = parse(args);
    std::optional<fs::path> pd = cli.profiles_dir ? cli.profiles_dir : profiles::default_profiles_dir();
    if (!pd) throw std::runtime_error("cannot work out a profiles folder; pass --profiles-dir");
    const fs::path profiles_dir = *pd;
    const std::vector<std::string>& c = cli.command;
    auto is = [&](std::initializer_list<const char*> words) {
        if (c.size() != words.size()) return false;
        size_t i = 0;
        for (const char* w : words) {
            if (w && c[i] != w) return false;
            ++i;
        }
        return true;
    };

    if (c.empty() || is({"help"})) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (is({"list"})) {
        auto entries = profiles::list(profiles_dir);
        if (entries.empty()) std::printf("no profiles in %s\n", ustr(profiles_dir).c_str());
        for (const auto& e : entries) {
            if (e.profile) {
                const pk::Profile& p = *e.profile;
                std::printf("%s %s %s %zu patch(es)  %s\n", pad(e.name, 20).c_str(), pad(p.server ? *p.server : "-", 24).c_str(),
                            pad(p.local_server ? "local server" : "client only", 13).c_str(),
                            patching::summary(e.dir, p).patches, p.name.c_str());
            } else {
                std::printf("%s INVALID: %s\n", pad(e.name, 20).c_str(), e.error.c_str());
            }
        }
        return 0;
    }
    if (is({"info"})) {
        std::printf("profiles: %s\n", ustr(profiles_dir).c_str());
        const Setup setup = resolve(cli.opts);
        std::printf("game:     %s\n", ustr(setup.game_dir).c_str());
        std::printf("tools:    %s\n", ustr(setup.tools_dir).c_str());
        if (setup.proton) {
            std::printf("proton:   %s\n", ustr(setup.proton->dir).c_str());
            std::printf("runtime:  %s (Proton runs inside this container)\n", ustr(setup.proton->runtime).c_str());
            std::printf("prefix:   %s\n", ustr(setup.proton->compat_data).c_str());
        }
        if (setup.proton) std::printf("runner:   steam-proton (deprecated; umu is the default)\n");
        if (setup.wine) {
            std::printf("runner:   %s%s\n", ustr(setup.wine->program).c_str(), setup.wine->umu ? " (umu)" : "");
            if (setup.wine->umu)
                std::printf("proton:   %s\n", setup.wine->proton ? ustr(*setup.wine->proton).c_str() : "umu's own (downloaded on first use)");
            std::printf("prefix:   %s\n", ustr(setup.wine->prefix).c_str());
        }
        return 0;
    }
    if (is({"run", nullptr})) {
        const std::string& name = c[1];
        check_profile_name(name);
        const fs::path dir = profiles_dir / upath(name);
        const Setup setup = resolve(cli.opts);
        prepare::Prepared prepared = prepare::prepare(setup, dir, cli.game_args, [](const prepare::Event& e) {
            if (e.kind == prepare::Event::Note) std::fprintf(stderr, "note: %s\n", e.text.c_str());
            return std::optional<std::string>();
        });
        std::printf("starting %s with profile %s%s (%s)\n", ustr(setup.game_exe()).c_str(), name.c_str(),
                    prepared.profile.server ? (" (server " + *prepared.profile.server + ")").c_str() : "",
                    describe_trust(prepared.trust).c_str());
        std::fflush(stdout);
        std::optional<int> code;
        try {
            Process game = spawn(prepared.command);
            // Steam Deck Game Mode: tag the game's windows so gamescope shows them.
            std::optional<gamescope::WindowTagger> tagger;
            if (auto app = gamescope::app_id()) tagger.emplace(*app);
            code = game.wait();
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("could not start the injector: ") + e.what());
        }
        try {
            profiles::sync_shared(dir, prepared.profile, prepared.shared);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "warning: could not save game settings back: %s\n", e.what());
        }
        if (code != 0)
            if (auto e = inject_error(dir)) std::fprintf(stderr, "sc-launch: the game did not start: %s\n", e->c_str());
        std::printf("agent log: %s\n", ustr(dir / pk::kAgentLog).c_str());
        return code ? std::min(std::max(*code, 0), 255) : 1;
    }
    if (is({"install", nullptr})) {
        const packs::Staged staged = packs::stage(packs::Source::parse(c[1]), profiles_dir);
        return install_reviewed(staged, profiles_dir, cli.yes);
    }
    if (is({"updates"})) {
        bool any = false;
        for (const auto& e : profiles::list(profiles_dir)) {
            if (!e.pack) continue;
            try {
                if (auto a = packs::check_update(*e.pack)) {
                    any = true;
                    std::printf("%s %s -> %s\n", pad(e.name, 20).c_str(), e.pack->version.c_str(), a->version.c_str());
                }
            } catch (const std::exception& err) {
                std::fprintf(stderr, "%s: could not check for updates: %s\n", e.name.c_str(), err.what());
            }
        }
        if (!any) std::printf("all packs are up to date\n");
        return 0;
    }
    if (is({"update", nullptr})) {
        const std::string& name = c[1];
        check_profile_name(name);
        const pk::Pack pack = profiles::read_pack(profiles_dir / upath(name));
        auto a = packs::check_update(pack);
        if (!a) {
            if (!pack.update) throw std::runtime_error(name + " has no update address");
            std::printf("%s is up to date (%s)\n", name.c_str(), pack.version.c_str());
            return 0;
        }
        packs::Source src;
        src.kind = packs::Source::Url;
        src.url = a->url;
        return install_reviewed(packs::stage(src, profiles_dir), profiles_dir, cli.yes);
    }
    if (is({"patches", nullptr})) {
        check_profile_name(c[1]);
        const fs::path dir = profiles_dir / upath(c[1]);
        const mods::List list = mods::client(dir, profiles::load(dir));
        print_toggles(list);
        for (const std::string& e : list.errors) std::printf("problem: %s\n", e.c_str());
        for (const std::string& w : list.warnings) std::printf("warning: %s\n", w.c_str());
        std::printf("* = your switch; the rest follow the pack\n");
        return list.errors.empty() ? 0 : 1;
    }
    if (is({"patches", nullptr, nullptr, nullptr, nullptr})) {
        check_profile_name(c[1]);
        const std::optional<bool> on = switch_value(c[2]);
        const fs::path dir = profiles_dir / upath(c[1]);
        mods::set_client(dir, c[3], c[4], on);
        std::printf("%s: %s %s is %s\n", c[1].c_str(), c[3].c_str(), c[4].c_str(),
                    on ? (*on ? "on" : "off") : "back to the pack's default");
        warn_clashes(mods::client(dir, profiles::load(dir)), c[3], c[4]);
        return 0;
    }
    if (is({"server", "status", nullptr})) return server_status(upath(c[2]));
    if (is({"server", "install", nullptr}) || is({"server", "install", nullptr, nullptr})) {
        std::optional<fs::path> proxy;
        if (c.size() == 4) proxy = upath(c[3]);
        else {
            std::optional<fs::path> tools = cli.opts.tools_dir ? cli.opts.tools_dir : env::path(env::kToolsDir);
            if (!tools && current_exe().has_parent_path()) tools = current_exe().parent_path();
            proxy = mods::find_proxy(tools);
            if (!proxy)
                throw std::runtime_error("no server proxy build found (pass it, or set " + std::string(env::kServerProxy) + ")");
        }
        const auto before = mods::install_proxy(upath(c[2]), *proxy);
        std::printf("%s %s\n", before == mods::ServerStatus::Installed ? "updated the proxy from" : "installed the proxy from",
                    ustr(*proxy).c_str());
        if (before == mods::ServerStatus::Restored) std::printf("the game's DLL (put back by Steam) is the new original\n");
        std::printf("restart the server for it to take effect\n");
        return 0;
    }
    if (is({"server", "start", nullptr})) {
        const fs::path cloud = upath(c[2]);
        if (servers::run_state(cloud, std::time(nullptr)).state == servers::RunState::Running)
            throw std::runtime_error("the server in " + c[2] + " is already running");
        const Command cmd = server_command(resolve(cli.opts), cloud, "run");
        const fs::path out = mods::server_root(cloud) / "logs" / "sc-server.stdout";
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        spawn(cmd, Output{Output::Kind::File, out}, true);
        std::printf("starting the server in %s ...\n", c[2].c_str());
        for (int i = 0; i < 240; ++i) {  // a minute
            if (servers::run_state(cloud, std::time(nullptr)).state == servers::RunState::Running) {
                std::printf("server running; Hub is starting its services (log: %s)\n",
                            ustr(mods::server_root(cloud) / "logs" / "sc-server.log").c_str());
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        throw std::runtime_error("the server did not start; see " + ustr(mods::server_root(cloud) / "logs" / "sc-server.log") +
                                 " and " + ustr(out));
    }
    if (is({"server", "stop", nullptr})) {
        const fs::path cloud = upath(c[2]);
        const Command cmd = server_command(resolve(cli.opts), cloud, "stop");
        const std::optional<int> code = spawn(cmd, Output{Output::Kind::Inherit, {}}, true).wait();
        return code.value_or(1);
    }
    if (is({"server", "logs", nullptr}) || is({"server", "logs", nullptr, nullptr})) {
        const fs::path cloud = upath(c[2]);
        std::optional<fs::path> local;
        try {
            local = profiles::shared_local_appdata(resolve(cli.opts));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "note: the game's own logs are left out: %s\n", e.what());
        }
        const std::vector<servers::LogFile> all = servers::logs(cloud, local);
        if (c.size() == 3) {
            for (const servers::LogFile& f : all) {
                char when[32] = "";
                const std::time_t t = std::time_t(f.modified);
                std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&t));
                std::printf("%-9s %-24s %9llu  %s  %s\n", f.group.c_str(), f.label.c_str(), static_cast<unsigned long long>(f.size),
                            when, f.name.c_str());
            }
            if (all.empty()) std::printf("no logs yet\n");
            return 0;
        }
        auto lower = [](std::string s) {
            for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        };
        for (const servers::LogFile& f : all)
            if (lower(f.name) == lower(c[3]) || lower(f.label) == lower(c[3])) {
                bool cut = false;
                const std::string text = servers::log_tail(f.path, 64 * 1024, &cut);
                if (cut) std::printf("... (the last 64 KB of %s)\n", ustr(f.path).c_str());
                std::fwrite(text.data(), 1, text.size(), stdout);
                return 0;
            }
        throw std::runtime_error("no log " + pk::serde::debug_str(c[3]) + " (run `sc-launch server logs " + c[2] + "` for the list)");
    }
    if (is({"server", "uninstall", nullptr})) {
        mods::uninstall_proxy(upath(c[2]));
        std::printf("the game's own %s is back (starforge/ is left in place)\n", mods::kProxyDll);
        return 0;
    }
    if (is({"server", nullptr, nullptr, nullptr, nullptr}) && c[1] != "status") {
        const std::optional<bool> on = switch_value(c[1]);
        mods::set_server(upath(c[2]), c[3], c[4], on);
        std::printf("%s %s is %s; restart the server for it to take effect\n", c[3].c_str(), c[4].c_str(),
                    on ? (*on ? "on" : "off") : "back to its default");
        warn_clashes(mods::server(upath(c[2])), c[3], c[4]);
        return 0;
    }
    if (is({"pack", "check", nullptr})) {
        const packs::Staged staged = packs::stage(packs::Source::parse(c[2]), profiles_dir);
        print_review(staged, existing_pack(profiles_dir, staged.pack.id));
        std::vector<std::string> problems;
        for (const auto& l : patching::load_sets(staged.root))
            if (!l.set) problems.push_back(l.error);
        try {
            const fs::path game_dir = find_game(cli.opts);
            const pk::Profile profile = pk::Profile::from(staged.pack, pk::Local{});
            try {
                for (const std::string& w : patching::preflight(staged.root, profile, game_dir / "win64" / "game.exe"))
                    std::printf("  note: %s\n", w.c_str());
            } catch (const std::exception& e) {
                problems.push_back(e.what());
            }
        } catch (const std::exception& e) {
            std::printf("  (patches not checked against the game: %s)\n", e.what());
        }
        packs::discard(staged);
        for (const std::string& p : problems) std::printf("  problem: %s\n", p.c_str());
        std::printf("%s\n", problems.empty() ? "pack OK" : "pack has problems");
        return problems.empty() ? 0 : 1;
    }
    if (is({"pack", "keygen", nullptr})) {
        const std::string& file = c[2];
        const fs::path path = upath(file);
        std::error_code ec;
        if (fs::exists(path, ec)) throw std::runtime_error(file + " already exists; not overwriting a key");
        const signing::SecretKey key = signing::SecretKey::generate();
        write_secret(path, key.to_text());
        std::printf("signing key written to %s; keep it secret and backed up\n", file.c_str());
        std::printf("public key:  %s\n", key.public_key().to_string().c_str());
        std::printf("fingerprint: %s\n", key.public_key().fingerprint().c_str());
        return 0;
    }
    if (is({"pack", "files", nullptr, nullptr})) {
        const std::string& folder = c[2];
        const std::string& out = c[3];
        std::string lower = out;
        for (char& ch : lower)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        const bool scmod1 = lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".txt") == 0;
        const std::string text = remote::build_list(upath(folder), cli.base_url, scmod1);
        profiles::write_atomic(upath(out), text);
        size_t count = 0, start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            const std::string line = text.substr(start, end - start);
            if (line.find("sha256") != std::string::npos || line.find('\t') != std::string::npos) ++count;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        std::printf("wrote %s: %zu file(s), %s\n", out.c_str(), count, scmod1 ? "SCMOD1" : "STAR FORGE list");
        if (auto key = signing_key(cli, false)) {
            profiles::write_atomic(upath(out + ".sig"), signing::sign_bytes(*key, text));
            std::printf("signed it with key %s (%s.sig); upload both\n", key->public_key().fingerprint().c_str(), out.c_str());
        }
        return 0;
    }
    if (is({"pack", "sign", nullptr})) {
        auto key = signing_key(cli, true);
        const signing::Manifest m = signing::sign(upath(c[2]), *key);
        std::printf("signed %s %s (%zu files) with key %s\n", m.id.c_str(), m.version.c_str(), m.files.size(),
                    key->public_key().fingerprint().c_str());
        return 0;
    }
    throw std::runtime_error("unknown command; see sc-launch --help");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // The command line as UTF-8 (argv is in the ANSI code page), and UTF-8 out.
    (void)argv;
    int n = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; wargv && i < n; ++i) args.push_back(ustr(fs::path(wargv[i])));
    LocalFree(wargv);
    SetConsoleOutputCP(CP_UTF8);
#else
    args.assign(argv + 1, argv + argc);
#endif
    env::load_dotenv();
    try {
        return run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sc-launch: %s\n", e.what());
        return 1;
    }
}
