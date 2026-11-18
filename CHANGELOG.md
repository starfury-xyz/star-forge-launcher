# Changelog

Each release's section becomes the notes of its GitHub release draft, so add
to **Unreleased** as you go and rename it to the version when you tag
(`## 0.2.2`, matching the tag without the `v`).

## Unreleased

## 0.3.1

- **Save diagnostics** (Settings → Star Conflict → Folders and logs, or
  `sc-launch diagnose <zip>`): saves a zip, wherever you pick (Downloads is
  offered first), to send when asking for help. It holds a report of the launcher, the game setup and every
  profile (can its pack.json be read, parsed and saved, and if not, exactly
  why), plus the launcher's, profiles' and game's logs. Home folder paths are
  written as ~; the game's chat log is left out.
- **Clearer file errors on Windows.** A file the launcher can't open now
  says why: "Access denied" (permissions, or a folder made while running as
  administrator), or "Sharing violation" (another program, such as a scanner
  or sync tool, has it open), with what to try, instead of "permission
  denied (os error 13)" for both. A file another program holds for a moment
  is tried again for about a second first. Windows errors are always in
  English.
- **Paths show as they are.** Error messages no longer capitalized
  "starforge" and "starfury" inside paths and links (a profiles folder
  `D:\starforge_profiles` showed as `D:\STAR FORGE_profiles`), and editing a
  profile no longer changes its stored name or server to capitals.

- **Pre-release updates** (Settings → Launcher → Updates): testers can get
  launcher test versions (tags like `v0.3.1-rc.1`, published as GitHub
  pre-releases) before everyone else. Off by default; turning it off waits for
  the next full release.
- **Packs show their release number.** A pack whose file list carries a
  version (Star Fury's v1, v2, ...) now shows that, not the pack's own 1.0.x.
- **Folder settings are picked, not typed**: the game folder, Wine prefix and
  background folder have Browse… and Clear buttons, and only accept a folder
  that exists.
- **Open game logs** (Settings → Star Conflict → Folders and logs) opens the
  game's own logs folder for the selected profile.

- **Steam Deck Game Mode: the game no longer hangs at "Running" with no
  window.** Started from Steam, the launcher passed dozens of Steam's open
  handles on to the game's runtime, which then waited forever. Programs the
  launcher starts now get only standard input/output.
- **Steam Deck Game Mode: the game window now shows** instead of the launcher.
  umu turns off Steam's overlay library in Game Mode, which is what tags game
  windows for the Deck's compositor; the launcher now tags them itself while
  the game runs (so in Game Mode "Exit launcher" keeps it open).
- **Linux: the game now runs through umu-launcher** (bundled) instead of
  Steam's own Proton setup. It uses umu's own Proton (UMU-Proton, the same
  for everyone; another build can be picked in Settings) in a prefix of the
  launcher's own, and no longer needs Steam to have started the game first or
  the game to be in a Steam library, so copied installs, non-Steam shortcuts
  (Steam Deck) and sandboxed Steam work. Your settings are copied over from
  Steam's prefix on the first start, which also downloads UMU-Proton and the
  Steam Linux Runtime once (about 2 GB unpacked). Needs Python 3.
- Linux and macOS: **Settings → Star Conflict → Run the game with**
  (`--runner`, `STAR_FORGE_RUNNER`) can also name a Wine-style command:
  system `wine`, a Wine build or a wrapper script (the default on macOS).
- **Deprecated:** the old Steam Proton setup is still there as the
  `steam-proton` runner and will be removed in the next release. With it,
  the launcher started by Steam as a non-Steam game keeps the shortcut's
  `SteamGameId`, so the overlay and Game Mode follow the game.

## 0.3.0

- **Rewritten in C++.** The launcher, `sc-launch`, the injector and the agent
  are now one C++ project (the Rust and Tauri version is kept at the
  `rust-final` tag). Your profiles, settings and pinned keys carry over
  unchanged, and the launcher looks and works as before.
- STARFURY and Star Conflict Classic Europe are in the server list from the
  start: pick one and press Play, and the launcher installs its pack first.
- **Add server profile** puts pack links and zips first; setting up a server
  by hand is folded away under *Set up a server yourself*.
- New **Settings → Experimental** (off by default) shows two new pages:
  **Mods**, to switch single patches of a profile or a server install on or
  off, and **Servers**, to run the game's own server stack from a server
  install, with a server patcher and its logs. Both are early and still
  changing; see docs/server-hosting.md.
- `sc-launch patches` lists and switches a profile's patches;
  `sc-launch server ...` does the same for server installs (experimental).
- Windows: the `.msi` is gone; use the `-setup.exe`. Updating from a 0.2
  `.msi` install installs the setup version for your user; the installer
  offers to remove the old copy (Windows asks for administrator rights).
- Linux: the `.deb` and `.rpm` no longer update themselves (update them the
  way you installed them); the AppImage still does. The launcher no longer
  needs the AppIndicator library for its tray icon.

## 0.2.6

- Star Forge dev is away so I'm releasing 0.2.6 myself;
- The application supports the random update to Star Conflict that went out 
  recently. 
- I'm definitely not versed in rust, so If you're reading this, just know 
  that i've been staring at this github actions script for an hour now and
  I really thought that the rust part would be the hard part. i'm starting to
  see spots. Love all of you. Goondnight. -starfury dev

## 0.2.5

- Installing or updating a pack works when the profiles folder is on a
  different drive from the launcher's data folder (Windows "cannot move the
  file to a different disk drive", os error 17): packs are now unpacked for
  review inside the profiles folder

## 0.2.4

- Game-file downloads finish all at once: files wait in a staging folder
  until every one has arrived and matched its checksum, so a failed or
  cancelled download never leaves a mix of old and new files (and finished
  files are reused on the next try)
- Only one download per profile at a time; installing or updating a pack
  waits for a launch's download, and the other way round
- Downloads retry passing network trouble twice, give up on a stalled
  server after 20 s, check the announced size first, and say what went wrong
  in plain words ("Can't reach …", "… arrived damaged")
- A file list whose signature can't be fetched because of the network now
  starts the game with the last good files, like an unreachable list
- Pack updates are checked again when the launcher comes back into view
  (at most every 30 minutes) and every few hours; profiles with an update say
  so in the profile list, and an update found at launch (including one the
  server requires) shows **Review update** right away
- `sc-launch run` and the launcher share one launch sequence
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
