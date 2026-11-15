# Server hosting (experimental)

> **Experimental.** Everything on this page is new, in active development and
> lightly tested. It is hidden in the launcher until you turn on **Settings →
> Experimental → Mods and server hosting**. Formats, commands and file
> locations may still change between versions; back up a server install before
> trying it, and don't rely on it for a live server yet.

Star Conflict ships its whole server stack in the game's `cloud/` folder
(Hub, MongoDB and the 32-bit services Hub starts). STAR FORGE can run that
stack, and patch the server programs the way packs patch the client, without
changing the files on disk.

## In the launcher

With the Experimental setting on, two pages appear:

- **Servers**: add a server install (its `cloud/` folder; *Use this game's
  cloud folder* fills it in), then start and stop it, install or remove the
  server patcher, and read the services' logs.
- **Mods**: switch single patches on or off, for a profile's client patches
  or for a server install's patchsets.

On Linux the server runs in the same prefix and with the same runner as the
game (by default umu with Proton, in the launcher's own prefix).

## On the command line

```sh
sc-launch server status <cloud-dir>          # proxy, exe builds and patches, checked as at startup
sc-launch server install <cloud-dir> [dll]   # put the server proxy in (the game's DLL is kept)
sc-launch server uninstall <cloud-dir>       # put the game's own DLL back
sc-launch server start <cloud-dir>           # MongoDB, then Hub and its services
sc-launch server stop <cloud-dir>            # Ctrl+C to Hub, then MongoDB
sc-launch server logs <cloud-dir> [log]      # list the logs, or show the end of one
sc-launch server on|off|default <cloud-dir> <exe> <id>   # switch a server patch
```

## Starting and stopping

`sc-server.exe` (in the launcher's `tools/`) does what the game client does
for its built-in local server: it starts MongoDB, unless its port is already
taken or `data/cloud.xml` points the database at another machine, then
`Hub.exe`, which starts the services listed in `cloud.xml`. The database lives
in the game's usual place (`%LOCALAPPDATA%\Targem\StarConflict\cloud\mongodb\data`).
Stopping sends Ctrl+C to Hub, then to anything Hub left running, then to
MongoDB; whatever doesn't stop in time is ended.

While it runs, `cloud/starforge/server.json` holds the processes and a
heartbeat. Logs:

- `cloud/starforge/logs/sc-server.log`: starting and stopping;
- `cloud/starforge/logs/<program>.log` (`Shard.log`, ...): what the proxy
  applied, per program;
- the game's own service logs, under
  `%LOCALAPPDATA%\Targem\StarConflict\cloud\Hub\logs`.

## The server patcher

Every server program loads the game's `libcrypto-1_1_tg.dll`. `server install`
renames it to `libcrypto-1_1_tg_orig.dll` and puts a stand-in in its place
that forwards everything to the original and, when each program starts:

1. reads `cloud/starforge/patcher.toml`;
2. checks the program against `cloud/starforge/patchsets/<Exe>.patchset.toml`
   (by sha1, and every enabled patch's original bytes) and patches it in
   memory;
3. serves changed game files from `cloud/starforge/overlay/`, laid out like
   the game folder, so a server's data changes need no edits to the install.

The proxy isn't in the release packages yet: build it with the `win32`
preset ([Development](development.md)) and point the launcher at it with
`STAR_FORGE_SERVER_PROXY=<path to build/win32/libcrypto-1_1_tg.dll>`, or put it
in `tools/server/` next to the launcher. Starting, stopping and logs work
without it.

Anything that doesn't match means the program **refuses to start**, so a
server never runs half patched by accident. Programs without a patchset run
unchanged. A Steam update or file check puts the game's DLL back; the Servers
page then says so, and **Install patcher** (or `server install`) puts the
proxy back in.

Patchsets use the same format as [client patches](client-patches.md) (see
`core-patches/game.exe.patchset.toml` for a complete one), with 32-bit
addresses. A write can also call a C++ *midhook* instead of replacing bytes;
midhooks are compiled into the proxy (`cmake --preset win32
-DSF_PROXY_HOOKS=<folder of .cpp files>`), see `src/proxy/sfp_hook.hpp`.

### `patcher.toml`

```toml
on_error = "exit"          # or "continue": log and run with whatever verified

[files]                    # optional; relative to starforge/, or absolute
overlay = "overlay"        # changed game files
state = "state"            # what the servers write over game files

[stack]                    # optional: main thread stack in MB
"Shard.exe" = 8

[patches."DedicatedServer.exe"]
"my-patch" = false         # overrides the patch's default
```

Every program reads it at startup, so changes need a server restart but no
rebuild.
