# STAR FORGE server pack template

Copy this folder to start a pack for your server. The full guide, from here to
a signed pack with updates, is [docs/making-a-pack.md](../../docs/making-a-pack.md).

```
my-server/
├── pack.json        what the pack is and how to launch it
├── resources/       launcher artwork: logo, icon, background
├── files/           game files, laid out like the game folder
└── patches/         client patches (optional, experimental)
```

1. Rename the folder after your server.
2. Edit `pack.json`: at least `id`, `name`, `version` and `server.address`
   (field reference: [docs/pack-format.md](../../docs/pack-format.md); editors
   read the `$schema` line and check it as you type).
3. Put your artwork in `resources/` ([sizes](resources/README.md)), or remove
   the `branding` entries you don't use.
4. Put replacement game files in `files/` at their path under the game
   folder, or take them from a hosted list instead
   ([docs/file-lists.md](../../docs/file-lists.md)). Delete what you don't use.
5. Check it, then try it in the launcher:

   ```sh
   sc-launch pack check my-server
   sc-launch install my-server
   ```

6. Sign and publish it: [docs/making-a-pack.md](../../docs/making-a-pack.md#3-sign-it).
