# Signing and trust

Packs change what the game loads, and with patches what it runs, so the
launcher needs to know that a pack and its updates come from the server team
and haven't been changed on the way.

## What a signature is

A signed pack carries two extra files:

- `manifest.json`: the pack's id and version, the public key that signed it,
  and every pack file (`pack.json`, `resources/`, `files/`, `patches/`) with
  its size and SHA-256.
- `manifest.sig`: an ed25519 signature over the exact bytes of
  `manifest.json`.

Keys are written `ed25519:<base64>`; people compare them by fingerprint, a
short form such as `3946-9c8b-8342-f0e1`. A pack whose game files come from a
[file list](file-lists.md) signs that list separately (`<list>.sig`) with the
same key.

```sh
sc-launch pack keygen my-server.key            # once; keep the file secret and backed up
sc-launch pack sign my-server --key my-server.key
sc-launch pack files my-files/ manifest.json --key my-server.key
```

In CI, put the key file's text in a secret and pass it as
`STAR_FORGE_PACK_KEY` instead of `--key`.

## What the launcher checks

| When | What |
|---|---|
| Install or update | Every file against the signature; no extra or missing files; the key against the one pinned to the pack id. For a file list: its signature, then every downloaded file against the list. |
| First signed install | The key is **pinned** to the pack id (trust on first use). |
| Every later update | Must be signed by the pinned key. An unsigned copy of a signed pack is refused too. |
| Before every launch | `pack.json`, `patches/` and plugins are hashed again; `files/` is checked by size; a file list is re-fetched and its signature checked. |
| Profile removed | Its pin is dropped, so a reinstall starts fresh. |

Unsigned packs still install, with a note in the review; if they patch the
game's code, the note is a warning.

## What players see

The install review has a **Signature** row:

- *Not signed*.
- *Key …, new: updates must be signed with it from now on*: the first install.
- *Key …, same as the installed version*: an update from the same publisher.
- *<Publisher> (key …)*: a publisher the launcher knows (below).

A signature says who made a pack, not that it is safe; the review says so for
packs that patch the game.

## Known publishers

The launcher can ship server teams' keys (`kKnown` in
[`src/launch/trust.cpp`](../src/launch/trust.cpp)): a pack
whose id starts with a known publisher's prefix must be signed with their key,
even on first install, and the review shows their name. Server teams can add
theirs by pull request. The list is empty for now.

## Losing or changing a key

Players' launchers only accept updates signed with the pinned key. A server
that loses its key, or needs a new one, has to tell players to remove the
profile and install the pack again; there is no way around that by design.
Keep the key backed up somewhere safe.

## The launcher itself

Launcher updates are signed separately, with the STAR FORGE release key (see
[Development](development.md#releasing)).
