# Making a server pack

A server pack is everything a player needs to play on your server: where to
connect, which game files to replace, how the launcher looks while your
server is selected, and optionally patches to the game's code. Players add it
with one link and get your updates automatically.

```
my-server/
├── pack.json        what the pack is and how to launch it
├── resources/       launcher artwork: logo, icon, background
├── files/           game files, laid out like the game folder
└── patches/         client patches (optional, experimental)
```

Every field is described in [Pack format](pack-format.md). Working examples
are in [`samples/`](../samples/README.md).

## 1. Start from the template

1. Copy [`templates/pack`](../templates/pack) and rename it after your server.
2. Edit `pack.json`: at least `id` (a reversed domain such as
   `org.example.myserver`; never change it once published), `name`,
   `version` and `server.address`. Editors such as VS Code read the `$schema`
   line and offer autocomplete and error checking.
3. Put your artwork in `resources/` (sizes in
   [resources/README.md](../templates/pack/resources/README.md)), or remove the
   `branding` entries you don't use.
4. Put replacement game files in `files/`, at the path they have under the
   game folder: `files/data/gamedata/shared/quests.lua` replaces
   `<game>/data/gamedata/shared/quests.lua`. Remove the folder if you replace
   nothing.
5. Try it: `sc-launch install path/to/my-server`, or **Add server profile → Choose
   zip…** with the folder zipped. `sc-launch pack check path/to/my-server`
   shows what the launcher will make of it without installing it.

A pack that only sets a server (and maybe takes its files from a list, see
below) can be just a `pack.json`.

## 2. Choose how game files reach players

- **In the pack** (`files/`): simple; every change means a new pack version
  that players download whole.
- **From a file list** (`files_from`): you host the files and a list of them;
  the launcher downloads only what changed and refreshes them before each
  launch, so a changed file reaches players without a new pack version. See
  [File lists](file-lists.md). If you already publish an SCMOD1 list for
  another launcher, point the pack at it.

What players see is the same: the files live in their profile, never in the
game folder, so two servers that change the same file never clash.

## 3. Sign it

Signing lets players and the launcher tell that the pack and every update come
from you. Make a key once and keep it secret; back it up:

```sh
sc-launch pack keygen my-server.key     # prints the public key and its fingerprint
```

Sign after every change, before zipping:

```sh
sc-launch pack sign my-server --key my-server.key   # or STAR_FORGE_PACK_KEY=<the key file's text>
```

If your pack uses a file list, sign the list with the same key too. A signed
`pack.json` can't be changed by the launcher, so set `update.url` yourself.
Publish your key's fingerprint where players can compare it. Details:
[Signing and trust](signing-and-trust.md).

## 4. Publish it with updates

Host over HTTPS:

- the pack zip, and
- a copy of `pack.json` with a `download` entry naming the zip and its SHA-256:

  ```json
  "download": {
    "url": "https://example.org/star-forge/my-server-1.0.0.zip",
    "sha256": "…output of sha256sum my-server-1.0.0.zip…"
  }
  ```

  (A pack that is only a `pack.json` needs no zip and no `download`.)

Give players the link to that `pack.json`. They can paste it into the
launcher, or click a link on your site:

```
starforge://install?url=https://example.org/star-forge/pack.json
```

URL-encode the address if it has its own `?` or `&`. The launcher checks the
download against the SHA-256, shows the player what the pack does, and from
then on checks that `pack.json` for new versions.

To release an update: bump `version`, keep `id`, upload the new zip, and
update `download` in the hosted `pack.json`. Players keep their own changes
(name, address, options), which live in their `local.json`.

### Requiring an update

When players must update to keep playing, set `min_version` in the
**published** `pack.json`:

```json
"update": { "url": "https://example.org/star-forge/pack.json", "min_version": "1.3.0" }
```

Before each launch the launcher asks that address; anyone on an older version
is told to update first. If your site can't be reached, the game still
starts, and players are told the update check failed.

### Game builds

List the `game.exe` builds the pack was made for, so players with another
build get a warning:

```json
"requires": { "game": ["…sha256sum of win64/game.exe…"] }
```

## Things to know

- **`.pak` files win** unless you say otherwise. The game prefers a file inside
  a `.pak` archive over a loose file at the same path, so `files/` replaces
  files the game keeps loose (most of `data/gamedata`, UI scripts) or a whole
  `.pak`. To replace single packed files (such as the strings in
  `data/strings/`), add `"patches": {"core": ["loose-before-packed"]}`; see
  [Client patches](client-patches.md#core-patches).
- **Data the client and server share** (for example
  `data/gamedata/shared/*.lua`) should match your server's, or the client shows
  stock values your server doesn't use.
- **What a pack can contain:** `pack.json`, `manifest.json`/`manifest.sig`,
  `resources/`, `files/`, `patches/`, and top-level README/LICENSE/CHANGELOG
  files. Anything else in the zip (and any dotfile) is ignored.
- **Profiles keep separate game data.** The game's per-user data (store
  cache, notes, logs, local server database) is kept per profile, so your
  server never sees another server's cached data.
- **Players are trusting you.** Sign your pack and publish it from a source
  players can check, such as your website or repository.

To have a sample of your pack offered to every player under **Community
servers**, open a pull request adding it to [`samples/`](../samples).
