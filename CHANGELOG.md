# Changelog

Each release's section becomes the notes of its GitHub release draft, so add
to **Unreleased** as you go and rename it to the version when you tag
(`## 0.2.2`, matching the tag without the `v`).

## Unreleased

- First-run setup: **Express** (recommended folders, one click) or
  **Custom** (pick the Star Conflict folder, checked as you go, and the
  profiles folder, with an offer to move existing profiles). Settings →
  Star Conflict → **Run setup…** opens it again. As an AppImage it also
  offers to move itself into `~/Applications` with a menu entry, so
  `starforge://` links and self-updates keep working. Existing installs
  keep their folders and skip it
- The profiles folder can live anywhere; `sc-launch` uses the same one
  (`STAR_FORGE_PROFILES_DIR` overrides it)
- Closing the launcher exits by default (it used to go to the tray)
- The STARFURY community-server button installs STARFURY's own signed pack
- Star Conflict page: **Check for updates** for the selected pack (says when
  it's up to date, or why the check failed); a folder icon by the profile name
  opens its folder; one **Edit profile** button instead of two
- Tidier launch bar: no globe icon, and the status line only shows when it
  says more than the play button
- The notices bell is always in the rail; an empty history says so
- Menus (settings, profiles, pack review, downloads) slide out from the left
  rail and blur only the main area, so the rail and window controls stay
  usable and another menu opens in one click; Esc closes them
- Notifications stack in the bottom right, above any menu, at most three at a
  time; they clear after 5 s (errors 10 s, hovering holds them) and repeats
  count up. A bell in the rail opens the history
- The injector no longer opens a terminal window when the game starts, which
  on Linux also gave the game the wrong taskbar icon
- Home has an **Add a server or paste a pack link** button, so a pack link
  from Discord (where `starforge://` links can't be clicked) is easy to use
- A failed pack install offers **Try again** instead of a dead Install button
- Broken profiles can be selected, so they can be edited or deleted
- Game settings changed while the launcher was closed (launch behaviour
  "Exit") are shared with other profiles the next time the launcher starts
- Packs can't be installed, updated or deleted while the game is running
- Pack file lists and zips: names Windows would read as a different file
  (trailing dot or space, device names like `con`) are refused
- The launcher keeps working after an internal error instead of failing every
  later action
- Download window: when a pack's game files download (on install, or before
  a launch when the list changed), a window shows the percentage, a progress
  bar, speed, time left, a throughput graph and a log of each file as it
  arrives and is checked. It can be hidden (a ring in the left rail shows the
  progress and brings it back) or cancelled; files that finished are kept.
  Launcher updates download in the same window

## 0.2.3

- Linux: Proton now always runs inside the Steam Linux Runtime its build asks
  for. A build without one, or a missing runtime, is refused with a message
  instead of running Proton directly on the system
- When the game isn't installed, was never started from Steam, or the runtime
  is missing, the play button becomes **OPEN STEAM** and opens Steam to fix it
  (install the game, start it once, or install the runtime), then **RETRY**
- Notifications appear in the top right instead of over the page
- `sc-launch info` shows which runtime container Proton runs in

## 0.2.2

- New logo and emblem (SVG) and new app icons
- Settings: colour theme dropdown with swatches and a live preview on hover
- Dialogs keep their title and close button in place while the content scrolls
- Checkboxes are no longer cut off on Linux
- Signed server packs: server teams sign packs with `sc-launch pack sign`;
  the launcher checks every file, remembers the pack's key, refuses updates
  signed by anyone else, and re-checks the pack's code before each launch.
  The install review shows who signed a pack
- `sc-launch pack check` shows what a pack would do and checks it against the
  game without installing it; `sc-launch pack keygen` makes a signing key
- Each profile keeps its own game data (store cache, notes, logs, local
  server database), so servers no longer see each other's cached data.
  Graphics settings and key bindings are still shared; turn it off per
  profile in the profile dialog
- Packs can take their game files from a published file list instead of a
  zip: either a STAR FORGE list (JSON) or an SCMOD1 list, the format other
  community launchers use. Only new or changed files are downloaded, into the
  profile, and they're refreshed before each launch. `sc-launch pack files`
  writes either kind of list
- Core patches that ship with the launcher, turned on by a pack with
  `"patches": {"core": [...]}`. The first, `loose-before-packed`, lets a pack
  replace single files inside `.pak` archives (such as the game's strings)
- Community servers in one click: **Add profile** offers ready-made profiles
  for STARFURY and Star Conflict Classic Europe (kept in `samples/`)
- A published `pack.json` without a zip installs on its own, for packs that
  only set a server and take their game files from a list
- When the game can't start, the launcher now says why (the injector writes
  `inject.log` in the profile)
- The launcher's own `address.cfg` no longer trips a signed pack's check
- Servers can require a minimum pack version (`update.min_version` in the
  published `pack.json`); older versions must update before playing

## 0.2.1

- The launcher updates itself: it checks for a new version at startup (can be
  turned off in Settings, or checked by hand) and installs it and restarts.
  Update packages are signed. Install this version by hand; later ones arrive
  through the launcher
- Client patches: server packs can ship patchsets that are applied to the game
  in memory, checked against `game.exe` before launch, plus native plugins
- Processes the game leaves running (local server, MongoDB) are stopped when
  the game exits, so the launcher no longer stays on "Running"

## 0.2.0

- Server packs: import from a zip, folder, link or `starforge://` link, with a
  review before installing; checksum-verified pack updates that keep your own
  changes
- Per-pack branding (logo, icon, background, colour) and a background folder
  setting for your own wallpapers
- Mono app and taskbar icon

## 0.1.1

- STAR FORGE v1 art; the splash screen is now still
- Linux: Proton and links open with the desktop's environment, not the
  AppImage's

## 0.1.0

- First release: server profiles, Steam and Proton detection, game file
  overrides through the injected agent
