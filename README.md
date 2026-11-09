# starforge-cpp

C++ port of the [STAR FORGE](../star-forge) launcher, done one component at a time. The Rust
crates in `star-forge` stay the reference until each part here is a drop-in replacement.

| Component | Rust source | Status |
|---|---|---|
| `sc_agent.dll` (injected into game.exe) | `crates/sc-agent` | bootstrap, profile loading, game data folders, file redirects + folder listings, patches + plugins: done |
| `sf_core` (Windows lib: profile files as the agent reads them, Win32 helpers) | `crates/sc-profile` | what the agent and injector need (no full pack validation yet) |
| `sf_patch` (portable lib: patchset format, check/apply, core patches) | `crates/sc-patch` | done |
| `sf_launch` (portable lib: the launcher) | `crates/sc-launch` | env overrides, Steam/game/Proton detection, sc-inject command, process spawning, pack model (`src/pack`), profiles + settings sync, downloads (libcurl / WinHTTP), pack signatures (ed25519 via vendored Monocypher; verifies Rust-signed packs) done; pack install + trust, remote file lists, CLI to do |
| `sc-inject.exe` (starts game.exe with the agent) | `crates/sc-inject` | done (also double-checks a 0 LoadLibraryW result, which the Rust trusts) |

Everything builds as **Windows x64**: game.exe is 64-bit. You can build on Linux (cross-compile)
or on Windows (MSVC), and both use the same CMake project and the same `build/win64/` folder.

The launcher also runs **natively on Linux** (it starts the game through Proton), so its
portable code (`sf_patch`, `sf_launch`) has a third preset, `linux`, built with the system
compiler into `build/linux/`:

```sh
cmake --preset linux && cmake --build --preset linux && ctest --preset linux
```

Portable code keeps text as UTF-8 `std::string` and converts paths only with
`sf::launch::upath()` / `ustr()` (see `src/launch/paths.hpp` for why).

## Developer setup: Linux

Same toolchain as `star-forge/patcher`: `clang-cl` + `lld-link` cross-compiling against
Microsoft's CRT + Windows SDK, which [cargo-xwin](https://github.com/rust-cross/cargo-xwin)
downloads. Tests run under wine.

1. Tools:
   ```sh
   sudo pacman -S clang lld cmake ninja wine           # Arch / CachyOS
   sudo apt install clang lld cmake ninja-build wine   # Debian / Ubuntu
   ```
   `clang-cl`, `lld-link` and `llvm-rc` must be on `PATH` under those names (some distros
   only install versioned names like `clang-cl-18`; symlink them if so).
2. Microsoft CRT + SDK (one-time). If you've built the Rust launcher you already have them in
   `~/.cache/cargo-xwin/xwin`. Otherwise:
   ```sh
   cargo install cargo-xwin
   cargo xwin env --target x86_64-pc-windows-msvc >/dev/null
   ```
   This downloads Microsoft's packages and implies accepting their license. To use another
   location, pass `-DXWIN=/path/to/xwin` or set the `XWIN` env var.
3. Build and test (Windows build, plus the `linux` preset above for the launcher):
   ```sh
   cmake --preset win64
   cmake --build --preset win64     # -> build/win64/sc_agent.dll
   ctest --preset win64             # runs the test exes under wine
   ```

`patch_test` also checks the core patches against your installed `game.exe` (on Linux it finds the
Steam install itself; elsewhere pass `-DSF_GAME_EXE=C:/path/to/win64/game.exe`, or it skips that part).
`core-patches/` is a copy of star-forge's: when star-forge changes them, copy them over again.

Plugins include `src/agent/plugin_api.h` (plain C, same layout as the Rust agent's, so a plugin
works with either agent) and export `starforge_plugin_init`; `tests/test_plugin.cpp` is a minimal one.

xwin has no debug CRT, so every build uses the static release CRT (`/MT`), on both platforms.

## Developer setup: Windows

> The Windows preset has not been verified on a Windows machine yet. If something below
> fails, report the exact error so we can fix the preset or these notes.

1. Install **Visual Studio 2022** (Community is fine) or **Build Tools for Visual Studio 2022**
   with the **"Desktop development with C++"** workload. It includes MSVC, the Windows SDK,
   CMake and Ninja; nothing else is needed (no wine, no xwin).
2. Clone with Git for Windows. `.gitattributes` keeps files LF, so `core.autocrlf` doesn't
   matter.
3. Build and test, either way:
   - **Command line:** open **"x64 Native Tools Command Prompt for VS 2022"** (the normal
     prompt doesn't have `cl` on `PATH`), then:
     ```bat
     cmake --preset win64-msvc
     cmake --build --preset win64-msvc
     ctest --preset win64-msvc
     ```
   - **Visual Studio:** *File → Open → Folder…*, pick the repo. It reads `CMakePresets.json`;
     choose **win64-msvc** in the toolbar, then *Build → Build All* and *Test → Run CTests*.
   - **Cursor / VS Code:** see the next section. CMake Tools sets up the MSVC environment by
     itself; pick the **win64-msvc** preset when it asks.

## Editor (all platforms)

- **Cursor / VS Code:** the repo's `.vscode/` turns on CMake presets and turns off the
  built-in C++ IntelliSense (it doesn't understand the cross-compile and shows false
  errors). Install the recommended **clangd** and **CMake Tools** extensions when prompted.
- **clangd** (Cursor, VS Code, Kate, Neovim, Zed, ...): `.clangd` points it at
  `build/win64/compile_commands.json`, which configuring creates. Files show bogus errors
  (`windows.h` not found, ...) until you've configured once. After that, restart clangd.
- **Visual Studio:** works from the presets as-is; ignore `.clangd` and `.vscode/`.

## Tests

`tests/load_test.cpp` plays game.exe: it's built into `build/win64/fakegame/win64/`, loads
`sc_agent.dll` like sc-inject would, and checks what the agent did (its `agent.log`, and real
file/folder API calls). It runs one scenario per process (`load_test redirect`, ...), because a
DLL with live hooks must never be unloaded; ctest registers each as `agent_<scenario>`. Run one
with `ctest --preset win64 -R redirect -V`.

Wine is close to Windows but not identical: e.g. its `FindClose` accepts any handle, so a test
that relies on Windows rejecting a bad one can only fail on Windows. Run the tests on both.

Every ported step adds checks (or a new test) and is proven by breaking the code on purpose and
watching the right check fail, so each one works before the next starts.
