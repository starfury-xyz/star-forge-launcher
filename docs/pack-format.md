# Pack format

Pack format **1**. The machine-readable reference is
[`schemas/pack.schema.json`](../schemas/pack.schema.json); point `$schema` at
it and editors check your `pack.json` as you type.

## `pack.json`

Only `format`, `id`, `name` and `version` are required.

| Field | Meaning |
|---|---|
| `$schema` | Schema URL, for editors. Ignored by the launcher. |
| `format` | Always `1`. A launcher that's too old for a pack says so. |
| `id` | Unique pack id: lower-case letters and digits joined by `.` or `-`, usually a reversed domain (`org.example.myserver`). Updates and signing keys are tied to it, so never change it. |
| `name` | Shown in the launcher. |
| `version` | `x.y.z`. Updates are offered when the published version is higher. |
| `description` | One or two sentences for the pack card and review. |
| `authors` | List of names. |
| `links` | `website`, `discord`, `news`, `source`: buttons on the play screen. |
| `requires.launcher` | Oldest launcher version (`x.y.z`) that can use the pack. |
| `requires.game` | SHA-256 of the `win64/game.exe` builds the pack was made for; other builds get a warning. |
| `server.address` | Master server `host:port`. Players can override it. |
| `server.local` | Also start the game's built-in local server (MongoDB and Hub). |
| `game.args` | Extra arguments for `game.exe`. |
| `branding.logo` / `icon` | Images in `resources/`. |
| `branding.background` | An image in `resources/`, or a list to pick from at random. |
| `branding.accent` | Accent colour, `#rrggbb`. Replaces the player's theme colour while the pack is selected (players can turn pack styling off). |
| `files_from` | Take game files from a published list instead of `files/`: `{"manifest": url}` (STAR FORGE list) or `{"scmod1": url}`. See [File lists](file-lists.md). |
| `patches` | Client patches: `core` (launcher-provided patches by id, such as `loose-before-packed`), `method` (`inject`), `on_error` (`exit` or `continue`), `plugins` (DLLs in `patches/`). See [Client patches](client-patches.md). |
| `update.url` | The published `pack.json` the launcher checks for new versions. |
| `update.min_version` | In the published copy: the oldest version players may still play with. |
| `download` | Only in the published copy: `url` and `sha256` of the pack zip. |

## The profile folder

An installed pack becomes a profile folder. The pack's parts are replaced by
updates; everything else belongs to the player or the launcher.

```
profiles/my-server/
├── pack.json           ┐
├── manifest.json       │ the pack (replaced by updates)
├── manifest.sig        │
├── resources/          │
├── files/              │ game files: the pack's, or downloaded from its list
├── patches/            ┘
├── local.json          the player's own changes (never touched by updates)
├── remote-files.json   which files came from the pack's file list
├── state/              the game's per-user data for this profile
├── inject.log          starting the game, last launch
└── agent.log           inside the game, last launch
```

The launcher also writes `files/data/address.cfg` from the server address at
each launch.

## `local.json`

The player's changes on top of the pack. Most are set from **Edit profile**;
the rest can be added by hand.

| Field | Meaning |
|---|---|
| `name` | Display name instead of the pack's. |
| `server` | Server address instead of the pack's; `""` means none. |
| `local_server` | Override `server.local`. |
| `args` | Game arguments added after the pack's. |
| `shared_game_data` | `true`: use the game's normal per-user folder instead of the profile's `state/`. |
| `patches` | Turn single patches on or off: `{"game.exe": {"patch-id": false}}`. |
| `trace_files` | `true`: log every file the game opens to `agent.log` (large; for debugging). |
