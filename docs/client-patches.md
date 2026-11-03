# Client patches

A pack can patch the game's code as well as its files. **Experimental**: the
format of the files is settled (it's the one the
[STAR FORGE Patcher](../patcher/docs/FORMAT.md) uses for server binaries), but
how they are applied may still grow new methods.

Patches live in the pack's `patches/` folder:

- `<module>.patchset.toml`: byte patches for one module, usually
  `game.exe.patchset.toml`. One file per module.
- `*.dll`: native plugins, listed in `pack.json` under `patches.plugins`.

## Core patches

Some patches are useful to many packs, so they ship with the launcher instead
of every pack carrying the bytes. Their sources are in
[`core-patches/`](../core-patches) in this repository. A pack turns them on by
id:

```json
"patches": { "core": ["loose-before-packed"] }
```

They are applied exactly like a pack's own patchsets (checked against
`game.exe`, memory only, before the pack's own), players can switch them off
in `local.json` like any other patch, and the install review doesn't treat
them as the pack's own code.

| Id | What it does |
|---|---|
| `loose-before-packed` | Files in `files/` override files packed inside `.pak` archives. When the engine builds its file table it keeps the packed copy of a duplicate path; this makes it keep the loose copy instead, so a pack can replace single packed files such as `data/strings/<language>/string.txt`. A stock install has no duplicates, so without override files nothing changes. |

New core patches are added by pull request: a patchset in
`core-patches/<module>.patchset.toml` (with any assembly source next to it)
and its id in the schema.

## Patchsets

```toml
format = 1
exe = "game.exe"
sha1 = "…sha1sum of the pristine win64/game.exe…"  # or a list, if the patches hold for several builds
build = "1.99.0.167293"      # informational
image_base = 0x140000000     # addresses below assume this base

[[patch]]
id = "my-patch"              # players can turn patches on/off by id
title = "One line"
description = """Why and what, with RE notes."""
default = true               # on unless the player turns it off
optional = false             # true: a mismatch skips this patch instead of failing

[[patch.write]]
va = 0x140123456             # virtual address, image base included
orig = "0f8512000000"        # bytes that must be there now
bytes = "909090909090"       # replacement, same length
comment = "nop out the jne"
```

Every write lists the bytes it expects, so a patch never lands on the wrong
code. If `game.exe`'s sha1 differs from `sha1` (a game update), nothing is
applied. The launcher checks this before starting the game and when the pack
is installed. With `"on_error": "exit"` (the default) it refuses to start the game;
with `"continue"` it starts the game without the patches that don't match.

`midhook` writes (C++ hooks) are not supported for the 64-bit client yet; use
a plugin for hooks. Code caves can go in zero slack at the end of a section,
as with the server patcher.

## pack.json

```json
"patches": {
  "method": "inject",
  "on_error": "exit",
  "plugins": ["patches/my-hooks.dll"]
}
```

All of it is optional; the defaults are `inject` and `exit`, with no plugins.

- `method`: how patches are applied. `inject` is the only one so far: the
  launcher's agent DLL writes them into the game's memory while it is still
  suspended, and patches a DLL the moment it loads. The files on disk are
  never changed. Other methods (for example a patched copy of the
  executable) can be added later without changing patchsets.
- `plugins`: x64 DLLs the agent loads into the game after patching. A plugin
  can export `int starforge_plugin_init(const PluginApi *api)` (return 0 for
  success), with:

  ```c
  typedef struct {
      uint32_t version;                        /* 1 */
      const wchar_t *profile_dir;              /* valid during the call only */
      void (*log)(const char *utf8, size_t n); /* writes to agent.log */
  } PluginApi;
  ```

  The plugin runs under the loader lock before the game's own startup code,
  so keep `starforge_plugin_init` short: install hooks, start a thread if needed.
  Players are warned that the pack runs native code.

## Players

Players can turn single patches off in their profile's `local.json`, which
pack updates never touch:

```json
"patches": { "game.exe": { "my-patch": false } }
```

The agent logs what it applied (or why it didn't) to `agent.log` in the
profile folder.

## Limits

- Patchsets are tied to one `game.exe` build by its sha1; list that build in
  `requires.game` too, and check `agent.log` after a game update.
- `files/` can't replace a single file inside a `.pak` archive unless the pack
  turns on the `loose-before-packed` core patch.
