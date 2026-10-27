# Getting started

## Install

1. Install **Star Conflict** through Steam. On Linux, start it from Steam once
   so its Proton prefix exists.
2. Download STAR FORGE Launcher from the
   [Releases page](https://github.com/starfury-xyz/star-forge-launcher/releases):

   | Platform | File |
   |---|---|
   | Windows | `-setup.exe` (or `.msi`) |
   | Linux | `.AppImage` (any distro), `.deb` or `.rpm` |

3. Start it. It finds the game in your Steam libraries and, on Linux, the
   Proton build, Steam Linux Runtime and prefix Steam uses for it. If
   something is missing (the game isn't installed, it was never started from
   Steam, or the runtime isn't installed), the launcher says so and the play
   button becomes **OPEN STEAM**, which opens Steam to fix it; press **RETRY**
   afterwards. The game
   folder can also be set under **Settings → Star Conflict**.

The launcher updates itself: it checks at startup and offers to install a new
version and restart (turn this off, or check by hand, under **Settings →
Updates**). An AppImage must sit somewhere you can write to; Gear Lever's
`~/AppImages` is fine. `.deb` and `.rpm` updates ask for your password.

## Add a server

Each server you play on is a **profile**. Open **Add profile** (the + next to
the server list) and pick one of:

- **Community servers**: one click for the servers that ship with the
  launcher. STARFURY installs the server's own signed pack.
- **A link** from the server's website or Discord, pasted into the box
  (`https://…/pack.json` or `starforge://install?url=…`). Clicking a
  `starforge://` link in your browser opens the launcher directly.
- **A zip** the server gave you: **Choose zip…**.
- **Set one up yourself**: a name and a server address (`host:port`). Tick
  *Start the local server* only for the game's own built-in server.

Before anything is installed you see what the pack changes: the server, the
game files it replaces, whether it patches the game's code, who signed it,
and where its files come from. Only install packs from servers you trust:
they change what the game loads.

## Play

Pick the profile in the list at the bottom and press **Play**. The launcher
checks the pack, fetches any changed game files, and starts the game through
Steam's own Proton (Linux) or directly (Windows). Your Steam install is never
modified: the profile's changes exist only while the game runs with it.

## Updates

- **Packs** check their server for new versions at startup. When one is
  available the profile shows **Review update**; your own changes to the
  profile (name, address, ticked options) are kept.
- Some servers require the newest version: the launcher then asks you to
  update before you can play.
- Packs that take their files from a list refresh them before each launch, so
  small server changes arrive without an update.

## Your settings and data

- **Settings → Launcher**: what happens after starting the game and when
  closing the window, the colour theme (hover to preview), whether the
  selected server's pack styles the launcher, your own background folder, and
  update checks.
- **Settings → Star Conflict**: the game folder and (Linux) the Proton build.
- **Game data per profile.** Each profile keeps the game's own per-user data
  (store cache, notes, logs, the local server's database) separately, so
  servers never see each other's data. Your graphics settings and key
  bindings follow you to every profile. To use the game's normal data folder
  for a profile (for example to keep an existing local server database),
  untick *Keep this server's game data separate* in **Edit profile**.

Profiles live in the launcher's data folder (**Settings → Star Conflict → Open
profiles folder**):

| Platform | Folder |
|---|---|
| Linux | `~/.local/share/star-forge/profiles/` |
| Windows | `%APPDATA%\star-forge\profiles\` |

A few extra options have no button yet and go in the profile's `local.json`;
see [Pack format](pack-format.md#localjson).

## When something goes wrong

The launcher shows why a start failed. For more detail, open the profile
folder (**Edit profile → Open folder**) and look at:

| File | What it tells you |
|---|---|
| `inject.log` | starting the game and loading the agent; the last `error:` line is why it didn't start |
| `agent.log` | inside the game: which files were redirected, which patches applied, the game data folder |
| `state/AppData/Local/Targem/StarConflict/logs/` | the game's own logs for this profile |

and at the launcher's `game-launch.log` (**Settings → Star Conflict → Open
last launch log**), which has Proton's output on Linux.

Common causes:

- **"…made for a different game build"**: the pack's patches don't match your
  `game.exe` (usually after a game update); wait for the server to update the
  pack.
- **"…signed with key X when you installed it, but this copy is signed with
  Y"**: an update didn't come from the same publisher. If the server really
  changed keys, remove the profile and install the pack again.
- **"Could not check … for updates"**: the server's site is unreachable, so
  you may be missing an update; the game still starts with what you have.
- **"…hasn't been started from Steam on this computer yet"** (Linux): press
  **OPEN STEAM**, let the game reach its login screen, close it, and press
  **RETRY**. That first Steam launch creates the game's prefix.
- **"…needs the Steam Linux Runtime…"** or **"…doesn't run inside the Steam
  Linux Runtime"** (Linux): install the runtime with **OPEN STEAM**, or choose a
  current Proton for Star Conflict in Steam (Properties → Compatibility).
