# STAR FORGE Launcher

A launcher for **Star Conflict** that lets you keep several server setups side by
side and switch between them with one click. Each *server profile* carries its
own server address, replacement game files and code patches. Nothing in your
Steam install is modified; the changes exist only while the game runs with that
profile.

STAR FORGE is built by the [STARFURY](https://starfury.xyz) community (an
unofficial Star Conflict server), but it is meant for anyone running or playing
on a Star Conflict server project.

> **Status:** early development (0.3). It works for day-to-day play on Linux
> (Proton) and Windows, but expect rough edges.

## Features

- **Server packs**: a server hands out one link (or zip) with its address,
  game files and look. The launcher shows what a pack changes before
  installing it, and keeps it updated. Community servers are in the server
  list from the start.
- **Nothing touches your install**: a profile's files are redirected in while
  the game runs, so servers that change the same files never clash, and each
  profile keeps its own game data (store cache, notes, local server database).
- **Signed packs**: server teams sign their packs; the launcher remembers each
  pack's key and refuses updates from anyone else.
- **File lists**: servers can host their game files as a list (STAR FORGE or
  the SCMOD1 format other launchers use) and players download only what
  changed.
- **Client patches** (experimental): byte patches applied to the game's memory
  before it runs, checked against your `game.exe`, plus native plugins. Core
  patches ship with the launcher, such as letting packs replace single files
  inside `.pak` archives.
- **Linux and Windows**: on Linux the game runs through the Proton build and
  prefix Steam uses, with the Steam overlay. The launcher updates itself.
- **Command line**: `sc-launch` for scripts and for server teams' tooling.
- **Server hosting** (experimental, off by default): run the game's own
  server stack from the launcher, patch the server programs in memory and
  read their logs; see [Server hosting](docs/server-hosting.md).

## Get started

Download the launcher from the
[Releases page](https://github.com/starfury-xyz/star-forge-launcher/releases)
(Windows `-setup.exe`; Linux `.AppImage`, `.deb` or `.rpm`), start it, pick
a server and press **Play**. Star Conflict must be installed through Steam.

## Documentation

- [Getting started](docs/getting-started.md): installing, adding servers,
  updates, troubleshooting.
- [Making a server pack](docs/making-a-pack.md): for server teams; start from
  [`templates/pack`](templates/pack) or the [`samples/`](samples/README.md).
- [Pack format](docs/pack-format.md), [File lists](docs/file-lists.md),
  [Signing and trust](docs/signing-and-trust.md),
  [Client patches](docs/client-patches.md),
  [Server hosting](docs/server-hosting.md),
  [Command line](docs/command-line.md), [How it works](docs/how-it-works.md).
- [Development](docs/development.md): building (C++, CMake), tests,
  repository layout, releases.

Everything is indexed in [docs/](docs/README.md).

## Goals and to-do

**Profiles for everyone**
- [x] Server packs: import from a zip, folder, link or `starforge://` link,
      with a review of what the pack changes before it's installed
- [x] Pack updates from the server's published `pack.json`, checksum-verified,
      keeping the player's own changes
- [x] Warn when a pack was made for a different game build
- [x] Per-pack branding: the selected pack's logo, icon, background (one, or
      a random pick from a list) and accent colour; can be turned off in Settings
- [x] Background folder setting: a random wallpaper of your own when the pack
      has no background
- [x] Client patches: server-patcher patchsets applied in memory by the agent,
      checked against `game.exe` before launch, plus native plugins
- [ ] `sfp build` for x64 sources (today client patchsets are written resolved)
- [ ] x64 midhooks in the agent (C++-style hooks without a plugin)
- [x] Let `files/` override members of `.pak` archives (`loose-before-packed`
      core patch)
- [ ] Settle on patch methods beyond `inject` if needed
- [x] Patch switches per profile (**Mods** page, `sc-launch patches`; experimental)
- [ ] Export a profile as a pack zip from the launcher
- [ ] Profile editor for patches and extra arguments in the UI
- [x] Pack signing: ed25519-signed manifests, key pinned per pack id on first
      install, pack code re-checked before every launch; `sc-launch pack
      keygen|sign|check` for server teams
- [x] Each profile keeps its own game data (store cache, notes, local server
      database); graphics settings and key bindings stay shared
- [x] Servers can require a minimum pack version before players launch
- [ ] Known publishers: ship server teams' keys with the launcher
- [x] Core patches that ship with the launcher and packs switch on by id
- [x] Packs can take their game files from a published file list (STAR FORGE
      JSON or SCMOD1), downloading only what changed

**Launcher**
- [x] STAR FORGE's own logo, icon and default theme (v1)
- [ ] Server status on the play screen (online / player count)
- [x] Launcher self-update via GitHub releases (signed; AppImage and Windows installer)
- [ ] Self-update for the deb and rpm packages
- [x] Theme picker with colour previews (was "Faction colour")
- [ ] macOS support (Proton/Wine path)

**Server hosting** (experimental)
- [x] Start and stop a server install's stack (MongoDB, Hub and its services),
      on Linux in the game's Proton prefix
- [x] Server proxy: patchsets, C++ midhooks and a file overlay for the 32-bit
      server programs, applied in memory
- [x] Server logs in the launcher
- [ ] Build patchsets from patch sources (code caves, `sfp build`)
- [ ] Ship the server proxy with the launcher's releases

**Project**
- [x] Open-source release (MIT)
- [x] CI tests on Linux and Windows; tagged releases with packaged builds
- [x] One C++ codebase for the launcher, CLI, injector and agent (0.3)
- [ ] A clang-format style, enforced in CI
- [ ] Code signing for Windows builds
- [x] Guides for players and server teams ([docs/](docs/README.md))
- [ ] Contributor guide

## Disclaimer

STAR FORGE is an unofficial fan project. It is not affiliated with or endorsed
by the developers or publishers of Star Conflict. Star Conflict and its assets
belong to their respective owners; this repository contains no game files.

## License

[MIT](LICENSE)
