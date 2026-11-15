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

It then starts `sc-inject.exe`: directly on Windows, and on Linux through the
same Proton, runtime and prefix Steam uses (below). The injector writes the server address
to the profile's `files/data/address.cfg`, starts `game.exe` suspended, loads
`sc_agent.dll` into it and resumes it. Everything the game starts (such as the
built-in local server) runs in a job object and is stopped when the game
exits. Afterwards the launcher copies changed settings back.

## Linux: Proton inside the Steam Linux Runtime

The launcher runs the game the way Steam does, with what Steam already set up:

- **Prefix:** the game's own, `steamapps/compatdata/212070`. Steam creates it
  on the first launch (and runs the game's first-time installers there); the
  launcher never makes one itself.
- **Proton:** the build Steam last ran the game with, from that prefix's
  `config_info`, unless one is chosen in Settings.
- **Runtime:** the Steam Linux Runtime that Proton build asks for in its
  `toolmanifest.vdf` (`require_tool_appid`), found in the Steam libraries.

The command is

```
SteamLinuxRuntime_4/run -- <proton>/proton waitforexitandrun sc-inject.exe --game … --profile … --dll …
```

with Steam's environment (`STEAM_COMPAT_DATA_PATH`, `SteamAppId`, …) and the
game, prefix, profile and tools folders shared into the pressure-vessel
container. Proton never runs outside the runtime: a build that names no
runtime, or whose runtime isn't installed, is refused with a message saying
what to do in Steam (and the play button opens Steam for it). `sc-launch info`
shows the Proton, runtime and prefix in use.

## Linux without Steam's Proton

**Settings → Star Conflict → Run the game with** (`--runner`,
`STAR_FORGE_RUNNER`) can name a Wine-style command instead: `wine`, a Wine
build, `umu-run`, or a wrapper script. The launcher then runs

```
<runner> sc-inject.exe --game … --profile … --dll …
```

with `WINEPREFIX` set to a prefix of its own (`prefix/` in the launcher's data
folder unless one is chosen), which Wine sets up on first use. Nothing comes
from Steam: no runtime, no overlay, no `compatdata`, and the game folder may be
anywhere. This suits a launcher started by Steam as a non-Steam game, a Steam
running in another container, or no Steam at all, and it is the default on
macOS. `umu-run` also gets `GAMEID=umu-212070` unless the environment sets one;
other variables (`PROTONPATH`, `WINEDLLOVERRIDES`, `DXVK_*`…) pass through.
Plain Wine draws with its own Direct3D unless DXVK is installed in the prefix.

The prefix is not Steam's, so the game's normal AppData there starts empty;
profiles that keep their own game data (the default) aren't affected.

When the launcher itself was started by Steam (as a non-Steam game, as on a
Steam Deck), the Proton path keeps Steam's `SteamGameId` so the overlay and
Game Mode's window focus follow the shortcut.

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
