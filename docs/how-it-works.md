# How it works

```
STAR FORGE Launcher ──▶ sc-inject.exe ──▶ game.exe (started suspended)
 (checks the pack,                          └─ sc_agent.dll
  syncs files)                                   ├─ applies the profile's patches
                                                 ├─ redirects file reads to <profile>/files/
                                                 └─ points the game's data folder at <profile>/state/
```

## Starting the game

Before a launch the launcher:

1. checks the pack against its signature and pinned key
   ([Signing and trust](signing-and-trust.md));
2. asks the pack's update address whether this version is still accepted;
3. refreshes files from the pack's file list, if it has one
   ([File lists](file-lists.md));
4. checks the pack's patches against the installed `game.exe`;
5. copies the player's graphics settings and key bindings into the profile.

It then starts `sc-inject.exe`: directly on Windows, and on Linux through a
runner (umu with Proton by default; below). The injector writes the server address
to the profile's `files/data/address.cfg`, starts `game.exe` suspended, loads
`sc_agent.dll` into it and resumes it. Everything the game starts (such as the
built-in local server) runs in a job object and is stopped when the game
exits. Afterwards the launcher copies changed settings back.

## Linux: runners

On Linux the launcher starts its Windows tools through a **runner**, chosen
under **Settings → Star Conflict → Run the game with** (`--runner`,
`STAR_FORGE_RUNNER`).

### umu (the default)

[umu-launcher](https://github.com/Open-Wine-Components/umu-launcher) runs
Proton inside the Steam Linux Runtime the way Steam does, without needing
Steam. The packages ship it as `tools/umu-run` (a pinned release; the system's
`umu-run` is used when the tools folder has none). It needs Python 3.

```
umu-run sc-inject.exe --game … --profile … --dll …
```

with `WINEPREFIX` set to the launcher's own prefix (`prefixes/umu` in its data
folder unless one is chosen) and `GAMEID=umu-212070`. Proton is umu's own
(UMU-Proton), so every player runs the same build; a Proton chosen in Settings
(`--proton`, `STAR_FORGE_PROTON`: Steam's, GE, CachyOS…) is passed as
`PROTONPATH` instead.

The first start downloads UMU-Proton (about 470 MB) and the Steam Linux
Runtime it needs, about 2 GB once unpacked and shared with other umu games
(the runtime in `~/.local/share/umu`; UMU-Proton in Steam's
`compatibilitytools.d` if there is one, where Steam and the Proton picker
also see it), so it takes longer and needs the internet. `PROTONPATH` and `GAMEID` set in the environment win.

Nothing else comes from Steam, so the game folder may be anywhere (a copied
install works; set it under **Settings → Star Conflict**), and it works with
the launcher started by Steam as a non-Steam game (Steam Deck) or with Steam
running in another container.

### Wine-style commands

Any other name or path is run as `<command> sc-inject.exe …` with
`WINEPREFIX` set (`prefixes/wine` unless one is chosen): `wine`, a Wine build,
or a wrapper script. Other variables (`WINEDLLOVERRIDES`, `DXVK_*`…) pass
through. Plain Wine draws with its own Direct3D unless DXVK is installed in the
prefix. This is the default on macOS.

### The game's data in the new prefix

The first time the game starts in a umu or Wine prefix, the launcher copies
the game's data (`AppData/Local/Targem/StarConflict`: settings, key bindings,
notes, the local server's database) over from Steam's prefix, if Steam ever ran
the game and the new prefix has none yet. It does this once per prefix
(`.starforge-steam-import` in the prefix records it). Profiles that keep their
own game data (the default) carry their settings with them anyway.

### steam-proton (deprecated)

The runner from 0.3 and earlier, to be removed in a later release: it reuses
everything Steam set up for the game.

- **Prefix:** the game's own, `steamapps/compatdata/212070`. Steam creates it
  on the first launch; the launcher never makes one itself.
- **Proton:** the build Steam last ran the game with, from that prefix's
  `config_info`, unless one is chosen in Settings.
- **Runtime:** the Steam Linux Runtime that Proton build asks for in its
  `toolmanifest.vdf` (`require_tool_appid`), found in the Steam libraries.

```
SteamLinuxRuntime_4/run -- <proton>/proton waitforexitandrun sc-inject.exe --game … --profile … --dll …
```

with Steam's environment (`STEAM_COMPAT_DATA_PATH`, `SteamAppId`, …) and the
game, prefix, profile and tools folders shared into the pressure-vessel
container. A build without a runtime, or whose runtime isn't installed, is
refused with a message saying what to do in Steam. When Steam itself started
the launcher (as a non-Steam game), Steam's `SteamGameId` is kept so the
overlay and Game Mode follow the shortcut.

`sc-launch info` shows the runner, Proton and prefix in use.

## The agent

The agent runs inside the game before any of the game's own code:

- **Patches.** It applies the pack's patchsets to the game's memory, checking
  every write's original bytes, and patches DLLs as they load. Then it loads
  the pack's plugins. See [Client patches](client-patches.md).
- **File redirection.** It hooks the Windows file functions (`CreateFile`,
  `GetFileAttributes`, `FindFirstFile`…). A path inside the game folder that
  also exists in the profile's `files/` opens the profile's copy, and folder
  listings include files only the profile has, because the engine finds loose
  files by listing `data\`. The game folder itself is never written to.
- **Game data folder.** The game asks Windows for its per-user folder once at
  startup (`SHGetFolderPath(CSIDL_LOCAL_APPDATA)`) and keeps everything there
  under `Targem\StarConflict`: store cache, notes, logs, and the local
  server's database (whose paths it passes on to those processes). The agent
  answers with `<profile>/state/AppData/Local`, so each profile has its own.

It logs what it did to the profile's `agent.log`.

## Limits

- A file inside a `.pak` archive wins over a loose file at the same path,
  unless the pack turns on the `loose-before-packed` core patch.
- Patches are tied to one `game.exe` build; after a game update the launcher
  refuses them rather than misapplying them.
