# File lists

A pack can take its game files from a list you host, instead of shipping them
in `files/`:

```json
"files_from": { "manifest": "https://example.org/files/manifest.json" }
```

```json
"files_from": { "scmod1": "https://example.org/sc-mods/manifest.txt" }
```

Set exactly one, and leave `files/` out of the pack. The launcher then:

- downloads only files that are new or changed, into the player's profile
  (never the game folder), and checks each against its SHA-256;
- removes files the list no longer names;
- refreshes them before every launch, so a changed file reaches players
  without a new pack version. If the list can't be reached, the game starts
  with the last complete set and a warning; a list or file that fails its
  checks stops the launch.

Paths are relative to the game folder (`data/gamedata/shared/quests.lua`), with
`/` or `\` separators. A list may not name `data/address.cfg`, which the
launcher writes from `server.address`.

Two formats are read, so a server can keep the one it already publishes. Both
will keep working while the community settles on one; if they are ever
merged, the old ones stay supported for a deprecation period.

## STAR FORGE list (JSON)

```json
{
  "format": 1,
  "base_url": "https://cdn.example.org/files/",
  "files": {
    "data/gamedata/shared/quests.lua": { "sha256": "…", "size": 48211 },
    "data/ui/textures/logo.dds": { "sha256": "…", "size": 349652, "url": "https://other.example.org/logo.dds" }
  }
}
```

- `files`: path → `sha256` (hex), `size` (bytes, optional but recommended:
  it is checked and shown to players before they install), and `url`
  (optional).
- A file's address is its own `url`, else `base_url` plus its path, else the
  list's own folder plus its path.
- Other fields (such as a `version` from your build tooling) are ignored.

## SCMOD1

The format the Star Conflict Europe community launcher uses. UTF-8 text:

```
SCMOD1
# comments and blank lines are ignored
data/gamedata/shared/quests.lua<TAB>sha256<TAB>https://example.org/sc-mods/data/gamedata/shared/quests.lua
```

The first line is `SCMOD1`; every other line is a path, its SHA-256 and its
HTTPS address, separated by tabs.

## Making a list

Lay out the changed files like the game folder, then:

```sh
sc-launch pack files my-files/ manifest.json                                       # STAR FORGE list, files next to it
sc-launch pack files my-files/ manifest.txt --base-url https://example.org/sc-mods/ # SCMOD1
```

A name ending in `.txt` writes SCMOD1, anything else the JSON list. Upload the
list and the files (keeping their paths) to where it says.

## Signing a list

A signed pack's list must be signed with the same key, as a detached
signature at the list's address plus `.sig` (`manifest.json.sig`,
`manifest.txt.sig`). Add `--key my-server.key` to `sc-launch pack files` and
upload the `.sig` it writes. Because the list holds every file's SHA-256, the
signature covers the files too. A list's signature is only checked for a
signed pack; an unsigned pack's files are checked against the list's hashes
only, and players are told so before they install.

Other launchers that read SCMOD1 can ignore the `.sig` file.
