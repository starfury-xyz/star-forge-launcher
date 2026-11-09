# starforge-cpp

C++ port of the [STAR FORGE](../star-forge) launcher, done one component at a time. The Rust
crates in `star-forge` stay the reference until each part here is a drop-in replacement.

| Component | Rust source | Status |
|---|---|---|
| `sc_agent.dll` (injected into game.exe) | `crates/sc-agent` | bootstrap only (env, log, game root) |
| injector | `crates/sc-inject` | not started |

## Toolchain (Linux)

Same as `star-forge/patcher`: `clang-cl` + `lld-link` (clang/lld packages) cross-compiling
against the MSVC CRT + Windows SDK fetched by [cargo-xwin](https://github.com/rust-cross/cargo-xwin),
plus `cmake` (3.21+) and `ninja`.

- x86_64 (default; game.exe and the launcher) uses cargo-xwin's default cache,
  `~/.cache/cargo-xwin/xwin`, which the Rust launcher build already fills.
- x86 (`-DSF_ARCH=x86`, server exes) uses `~/.cache/star-forge-xwin/xwin`, the patcher's cache.
- Override either one with `-DXWIN=/path/to/xwin` or the `XWIN` env var.

xwin has no debug CRT, so every build uses the static release CRT (`/MT`).

## Build

```sh
cmake --preset win64
cmake --build --preset win64     # -> build/win64/sc_agent.dll
```
