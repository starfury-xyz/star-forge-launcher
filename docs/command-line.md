# Command line

`sc-launch` does everything the desktop launcher does, for scripts and for
server teams. It ships in the `sc-launch_<version>_<platform>` archive on the
[Releases page](https://github.com/starfury-xyz/star-forge-launcher/releases),
together with `sc-inject.exe`, `sc_agent.dll` and `sc-server.exe`, which it
expects next to itself.

## Playing

```sh
sc-launch list                       # profiles, their servers and patches
sc-launch info                       # the detected game, tools and Proton
sc-launch run <profile> [-- args]    # start the game (args replace the profile's own)
sc-launch install <zip|folder|url> [--yes]   # install a pack, or update the profile that has it
sc-launch updates                    # check installed packs for new versions
sc-launch update <profile> [--yes]   # install the newest version of a profile's pack
sc-launch patches <profile>          # a profile's game patches and which are on
sc-launch patches <profile> on|off|default <exe> <id>   # switch one (saved in local.json)
```

`<profile>` is the profile's folder name, as `list` shows it.

## For server teams

```sh
sc-launch pack check <zip|folder|url>        # what a pack would do, checked as on install
sc-launch pack keygen <key file>             # make a signing key
sc-launch pack sign <folder> --key <key file>
sc-launch pack files <folder> <list> [--base-url <url>] [--key <key file>]
```

`pack check` also checks the pack's patches against your installed
`game.exe`. `pack files` writes an SCMOD1 list when `<list>` ends in `.txt`
(it then needs `--base-url`), otherwise a STAR FORGE list; see
[File lists](file-lists.md). The key can also come from the
`STAR_FORGE_PACK_KEY` environment variable (the key file's text).

## For server hosts (experimental)

```sh
sc-launch server status <cloud-dir>
sc-launch server install|uninstall <cloud-dir>
sc-launch server start|stop <cloud-dir>
sc-launch server logs <cloud-dir> [log]
sc-launch server on|off|default <cloud-dir> <exe> <id>
```

New and still changing; see [Server hosting](server-hosting.md).

## Options

| Option | Default |
|---|---|
| `--profiles-dir <dir>` | the launcher's data folder |
| `--game-dir <dir>` | found through Steam |
| `--tools-dir <dir>` | next to `sc-launch` |
| `--proton <dir>` | Linux: the Proton build that last ran the game |
| `--yes` | install without asking |

## Environment

Used when the matching option isn't given; builds configured with
`-DSF_DOTENV=ON` also read them from the repository's `.env` file.

| Variable | Overrides |
|---|---|
| `STAR_FORGE_GAME_DIR` | Star Conflict install folder |
| `STAR_FORGE_TOOLS_DIR` | folder with `sc-inject.exe`, `sc_agent.dll` and `sc-server.exe` |
| `STAR_FORGE_PROTON` | Proton build (Linux) |
| `STAR_FORGE_DATA_DIR` | launcher data folder (preferences, profiles, logs, pinned keys) |
| `STAR_FORGE_PACK_KEY` | pack signing key text, for `pack sign` and `pack files` |
| `STAR_FORGE_SERVER_PROXY` | the server proxy DLL `server install` uses (experimental) |
