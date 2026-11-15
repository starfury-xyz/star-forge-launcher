# Development

STAR FORGE is C++17, built with CMake. Everything the game loads is a Windows
program (`game.exe` is 64-bit, the server programs in `cloud/` are 32-bit), so
those parts always build for Windows: on Windows with MSVC, or on Linux by
cross-compiling with `clang-cl` and testing under Wine. The launcher and
`sc-launch` also build natively for Linux, where they start the game through
umu-launcher and Proton. The packages bundle a pinned umu (`packaging/fetch-umu.sh`);
a development build uses `umu-run` from `STAR_FORGE_TOOLS_DIR`, else the system's.

| Preset | Builds | Folder |
|---|---|---|
| `linux` | the launcher window and `sc-launch` for Linux, and the portable libraries' tests | `build/linux/` |
| `win64` | everything for 64-bit Windows, cross-compiled on Linux (tests run under Wine) | `build/win64/` |
| `win64-msvc` | the same on Windows with MSVC | `build/win64/` |
| `win32` | the 32-bit server proxy (`libcrypto-1_1_tg.dll`) and its tests | `build/win32/` |

```sh
cmake --preset linux && cmake --build --preset linux && ctest --preset linux
cmake --preset win64 && cmake --build --preset win64 && ctest --preset win64
```

A Linux launcher finds `sc-inject.exe`, `sc_agent.dll` and `sc-server.exe` in
`tools/` next to itself; for a development build point it at the Windows build
with `STAR_FORGE_TOOLS_DIR=$PWD/build/win64`.

## Setup on Linux

1. Tools:
   ```sh
   sudo pacman -S clang lld cmake ninja wine curl webkit2gtk-4.1          # Arch / CachyOS
   sudo apt install clang lld cmake ninja-build wine libcurl4-openssl-dev libwebkit2gtk-4.1-dev  # Debian / Ubuntu
   ```
   `clang-cl`, `lld-link` and `llvm-rc` must be on `PATH` under those names
   (some distros only install versioned names like `clang-cl-18`; symlink them
   if so). libcurl is only for the `linux` preset's downloads; the Windows
   build uses WinHTTP. WebKitGTK 4.1 (GTK 3) is only for the launcher window;
   without it the `linux` preset skips `star-forge`.
2. Microsoft's CRT and Windows SDK, fetched once with
   [cargo-xwin](https://github.com/rust-cross/cargo-xwin) (this downloads
   Microsoft's packages and implies accepting their license):
   ```sh
   cargo install cargo-xwin
   cargo xwin env --target x86_64-pc-windows-msvc >/dev/null    # for win64
   XWIN_ARCH=x86 XWIN_CACHE_DIR=~/.cache/star-forge-xwin cargo xwin env --target i686-pc-windows-msvc >/dev/null  # for win32
   ```
   The presets look in `~/.cache/cargo-xwin/xwin` (x64) and
   `~/.cache/star-forge-xwin/xwin` (x86). To use another location, pass
   `-DXWIN=/path/to/xwin` or set `XWIN`.

xwin has no debug CRT, so every build uses the static release CRT (`/MT`), on
both platforms.

## Setup on Windows

1. Install **Visual Studio 2022** (Community is fine) or **Build Tools for
   Visual Studio 2022** with the **Desktop development with C++** workload. It
   includes MSVC, the Windows SDK, CMake and Ninja.
2. Clone with Git for Windows. `.gitattributes` keeps files LF, so
   `core.autocrlf` doesn't matter.
3. Build and test, either way:
   - **Command line:** open **x64 Native Tools Command Prompt for VS 2022**,
     then `cmake --preset win64-msvc`, `cmake --build --preset win64-msvc`,
     `ctest --preset win64-msvc`.
   - **Visual Studio:** *File → Open → Folder…*, pick the repo, choose
     **win64-msvc** in the toolbar, then *Build → Build All* and
     *Test → Run CTests*.

The launcher window uses the WebView2 runtime that ships with Windows 10/11.

## Editors

- **VS Code / Cursor:** the repo's `.vscode/` turns on CMake presets and turns
  off the built-in C++ IntelliSense (it doesn't understand the cross-compile).
  Install the recommended **clangd** and **CMake Tools** extensions.
- **clangd** (any editor): `.clangd` points it at
  `build/win64/compile_commands.json`, which configuring creates. Until you've
  configured once, files show bogus errors (`windows.h` not found).

## Local configuration (`.env`)

Paths that differ per machine can be set through `STAR_FORGE_*` variables,
listed in [Command line](command-line.md#environment). Builds configured with
`-DSF_DOTENV=ON` also read them from the repository's `.env` (copy
`.env.example`; `.env` is gitignored); other builds only use the real
environment. Order of precedence: launcher settings and command-line flags,
then these variables, then auto-detection.

`STAR_FORGE_NO_REGISTER=1` stops a development build from registering itself
as the `starforge://` handler.

## Tests

Every test is a plain executable registered with CTest; `ctest --preset <p> -R
<name> -V` runs one with its output.

- `load_test` plays `game.exe`: it loads `sc_agent.dll` the way `sc-inject`
  would and checks what the agent did. It runs one scenario per process
  (`agent_<scenario>` in CTest), because a DLL with live hooks must never be
  unloaded.
- `patch_test` also checks the core patches against your installed `game.exe`
  (found through Steam on Linux; elsewhere pass
  `-DSF_GAME_EXE=C:/path/to/win64/game.exe`, or that part is skipped).
- `gui_test` drives every command the launcher's interface invokes, without
  a window.
- Wine is close to Windows but not identical (console control events, some
  handle checks), so CI runs the tests on real Windows as well.

## Repository layout

| Path | What |
|---|---|
| `src/agent` | `sc_agent.dll`, injected into the game: profile, game data folders, file redirects, patches, plugins |
| `src/inject` | `sc-inject.exe`, starts the game suspended and injects the agent |
| `src/launch` | the launcher's logic: Steam, game and Proton detection, profiles, packs, signing, trust, file lists, downloads, patch switches, server installs |
| `src/pack`, `src/profile` | the pack and profile formats |
| `src/patch` | patchset format, checking and applying, core patches, the server config (`patcher.toml`) |
| `src/redirect`, `src/hook` | file redirection and function hooking shared by the agent and the proxy |
| `src/common`, `src/crypto` | small shared helpers: files, text, base64, SHA-256, random bytes |
| `src/proxy` | the server proxy DLL (experimental; see [Server hosting](server-hosting.md)) |
| `src/server` | `sc-server.exe`, which starts and stops a server install (experimental) |
| `src/gui` | the launcher window's backend: every command the interface calls |
| `src/app` | the launcher window: webview host, tray, single instance, link registration, self-update |
| `src/cli` | `sc-launch`, the command-line launcher |
| `gui/ui` | the launcher's interface (HTML, CSS, JavaScript) |
| `samples/` | the community server packs listed in the launcher from the start (`kSamples` in `src/gui/profiles.cpp`) |
| `core-patches/` | core patchsets packs turn on by id, with their assembly sources; compiled into the launcher and agent |
| `schemas/` | the JSON schema of `pack.json` |
| `templates/pack/` | the pack template for server teams |
| `packaging/` | AppImage, deb, rpm and Windows installer scripts |
| `third_party/` | vendored libraries (each with its license) |
| `tests/` | the tests and their fake game, server and plugin programs |

Plugins include `src/agent/plugin_api.h` (plain C) and export
`starforge_plugin_init`; `tests/test_plugin.cpp` is a minimal one.

## Releasing

1. Rename the **Unreleased** section of [CHANGELOG.md](../CHANGELOG.md) to the
   new version (`## 0.3.1`); it becomes the release notes.
2. Set the same version in `CMakeLists.txt` (`project(starforge VERSION ...)`).
3. Push a tag like `v0.3.1`. GitHub Actions builds the Windows and Linux
   packages, signs them for the self-update and attaches them to a draft
   release with a `latest.json`.
4. Review the draft and publish it. Installed launchers only see published
   releases (they read `releases/latest/download/latest.json`).

The update signatures need two repository secrets: `TAURI_SIGNING_PRIVATE_KEY`
(the key file's contents) and `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`. The
public half is `SF_UPDATE_PUBKEY` in `CMakeLists.txt`; installed launchers
only accept updates signed with that key, so keep the private key and its
password safe. The names come from Tauri, which the launcher was first built
with: its signer (`npx @tauri-apps/cli@2 signer`) makes and uses the keys, and
launchers from before 0.3 update to 0.3 through the same `latest.json`. A fork
that publishes its own builds needs its own key pair, passed as
`-DSF_UPDATE_PUBKEY`, and its own `-DSF_UPDATE_ENDPOINT`.
