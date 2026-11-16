# Mods

> **Status: in progress** on the `mods` branch. Working: the loader
> (`mods/` folder, zips, order, switches, requirements, conflicts),
> [data patches](#data-patches), `scripts` in `mod.json`, whole files, and
> `sc-launch mods` / `sc-launch mod diff`. Not yet: [Lua hooks](#lua-hooks)
> (`sfmod.lua`), the launcher's Mods page, [pack restrictions](#packs-that-restrict-mods),
> patching files that only exist inside a `.pak`, [text diffs](#text-diffs).
> Sections marked *to verify* depend on things still to be tested in the game.

A mod is an optional, client-side change a player adds to one profile: a UI
layout, button prompts, icons, translations, textures. Players install mods
the way they would in a Minecraft instance: drop a zip in the profile's
`mods/` folder, turn it on, put it in order. STAR FORGE is the mod loader;
it puts the mods' files in front of the game's without touching the game
install, the same way it serves a pack's files.

Mods are built so that two of them can change the same game file without
fighting over it. Most conflicts between UI mods come from shipping a whole
copy of a game file to change a few lines in it; the formats below let a mod
say *what* it changes (one function, one texture entry, one layout object)
instead, and the loader combines them.

Mods are not packs. A pack is a server's own client setup and the server
depends on it; a mod is the player's choice and the server never knows about
it. Anything the server has to know about (new ships, modules, game modes)
belongs in a pack: see [Mods and the server](#mods-and-the-server).

## Installing mods

Each profile has its own `mods/` folder (**Mods → Open mods folder** in the
launcher). Put a mod in it as a `.zip` or as an unpacked folder; both work.
A mod you add is on, and goes last in the load order.

The player's choices live in the profile's `local.json`, which pack updates
never touch:

```json
"mods": {
  "order": ["retro-layout", "retro-icons", "deck-glyphs"],
  "off": ["retro-icons"]
}
```

- `order`: load order, by mod id. A mod that isn't listed loads after the
  listed ones, in alphabetical order of id. When two mods change the same
  thing, the later one wins (see [Conflicts](#conflicts-and-load-order)).
- `off`: mods that stay in the folder but aren't loaded.

The **Mods** page (to come) and the command line change these for you:

```sh
sc-launch mods <profile>                  # mods in load order, on/off, and why one didn't load
sc-launch mods <profile> on|off <id>
sc-launch mods <profile> order <id>...    # later ones win
sc-launch mods <profile> check            # load them as a start would; conflicts and problems
```

Each start, the launcher tells you about mods it couldn't load, patches it
left out and conflicts, and `agent.log` lists the loaded mods with a
fingerprint of the set (worth including in bug reports).

What the loader keeps in `mods/` (all of it rebuilt as needed; safe to
delete): `.cache/` (unpacked zips), `.merged/` (game files with the mods'
data patches applied), `.layers` (what the game is served from) and
`.loaded.json` (the last start's report).

Without STAR FORGE a player can still unzip a mod's `files/` folder into the
game folder by hand. Plain files work that way; [data patches](#data-patches),
[Lua hooks](#lua-hooks) and anything inside a `.pak` need the loader.

## Packs that restrict mods

> **Deferred.** Not part of the first version: every installed mod loads.
> This section is the plan for later.

A pack can limit which mods its players load (a competitive server, say), in
`pack.json`:

```json
"mods": { "allow": "all", "deny": ["some-mod"] }
```

| Field | Meaning |
|---|---|
| `allow` | `"all"` (default), `"none"`, or a list of mod ids: only these may load. |
| `deny` | Mod ids that may never load. Checked after `allow`. |

A mod the pack doesn't allow stays installed but isn't loaded, and the Mods
page says why. `pack.json` is read strictly, so a pack that uses `mods` needs
players on a launcher that knows the field (0.4.0 or newer, *to confirm at
release*).

## Making a mod

### Layout

```
retro-layout-1.0.0.zip
├── mod.json
├── files/          new files, laid out like the game folder
│   └── data/ui/mods/retro-layout/...
├── patches/        data patches (*.sfpatch) and text diffs (*.diff)
└── README.md       optional, shown on the Mods page
```

### `mod.json`

```json
{
  "format": 1,
  "id": "retro-layout",
  "name": "Retro equipment layout",
  "version": "1.0.0",
  "description": "Ships on the left, modules along the bottom, like 2014.",
  "authors": ["Com"],
  "links": { "source": "https://github.com/..." },
  "game": ["<sha1 of win64/game.exe>"],
  "needs": ["loose-before-packed"],
  "requires": { "sfmod": "1" },
  "after": ["retro-icons"],
  "scripts": [
    { "file": "ui\\mods\\retro-layout\\layout.lua",
      "after": "ui\\scripts\\windows\\equipmentwnd.lua" }
  ],
  "lua": { "replaces": ["UI.EquipmentWnd:LayoutPassiveShipSlots"] },
  "server": false
}
```

| Field | Meaning |
|---|---|
| `format` | Always `1` for now. |
| `id` | Lowercase letters, digits and `-`. Unique; names the mod everywhere (load order, pack rules, namespaces). |
| `name`, `version`, `description`, `authors`, `links` | Shown on the Mods page. `version` is semver. |
| `game` | Optional. `game.exe` builds the mod was made for; on another build it is skipped with a note. Leave it out for mods that don't depend on the build. |
| `needs` | Core patches the mod needs (see [Client patches](client-patches.md)). The loader turns them on while the mod is loaded. |
| `requires` | Other mods by id with a version range (`"retro-icons": ">=1.0"`), and `sfmod` (the [Lua API](#lua-hooks) version). A mod missing a requirement isn't loaded. |
| `after` / `before` | Load-order hints for when the player hasn't set an order. The player's `order` always wins. |
| `scripts` | Lua scripts to register. Shorthand for an `add` patch on `uiresources.txt`; see [Scripts](#scripts). |
| `lua.replaces` | Functions the mod [replaces](#lua-hooks), so the launcher can warn about a conflict before the game starts. |
| `server` | `true` if the mod changes anything the server must also have (see [Mods and the server](#mods-and-the-server)). Such a mod loads only when the pack lists it as required. Default `false`. |

## Three ways to change the game

Use the first one that can express your change:

1. **[Data patches](#data-patches)**: change named entries in the game's text
   data files (texture lists, layouts, resource lists).
2. **[Lua hooks](#lua-hooks)**: change or extend single functions of the UI
   scripts.
3. **[New files](#new-files)**: textures, scripts, atlases of your own, at
   your own paths.

And, as last resorts: [replacing a whole game file](#replacing-game-files)
and [text diffs](#text-diffs).

Never ship a modified copy of a game file when one of the first three can do
it. A copy conflicts with every other mod that touches that file, even when
the two change unrelated things.

## Data patches

Most of the game's data files (`uiresources.txt`, texture lists such as
`textures/common/*.txt`, `.sanim` layouts, and others) share one text format:
lines of `key value`, and blocks with a header and `{ }`:

```
Texture
{
	name		"ItemSubtype_FocusedRay"
	fileName	"ui\textures\ico\active_mod_icon_2"
	rect		"973 290 70 70"
}

objects 18
{
	object agen
	{
		defaults
		{
			anchor "LB"
			pos 17 799
		}
	}
}
```

A data patch is a `.sfpatch` file in `patches/`, written in that same format,
so values can be copied straight from the game file. The easiest way to make
one is to edit a copy of the game file, then let STAR FORGE write the patch:

```sh
sc-launch mod diff <stock file> <your edited copy> data/ui/textures/common/module_new.txt > patches/icons.sfpatch
```

It writes `set`/`unset` operations for changed, added and removed keys; blocks
you added or removed still have to be written by hand. A patch looks like this:

```
// patches/icons.sfpatch
patch "data/ui/textures/common/module_new.txt"
{
	set Texture[name="ItemSubtype_FocusedRay"]
	{
		fileName	"ui\textures\interation_2\textures_s2\item_icon_s6"
		rect		"255 294 40 40"
	}
}

patch "data/ui/graphics/equip_1080/equip_main2.sanim"
{
	set objects/object agen/defaults
	{
		pos 17 -799
	}
}
```

### Selectors

A selector picks blocks by path, from the top of the file:

| Selector | Picks |
|---|---|
| `Texture` | Top-level blocks whose header is `Texture`. |
| `objects/object agen/defaults` | The `defaults` block of `object agen` inside `objects 18`. A segment matches the header's first words, so `objects` matches `objects 18`. |
| `Texture[name="ItemSubtype_FocusedRay"]` | `Texture` blocks with a line `name "ItemSubtype_FocusedRay"` directly inside. |
| `Item[fileName="ui\scripts\work\uigamefuncs.lua"]` | The resource entry for that script. |
| `*` | Any header. |
| `.` | The top level of the file (for `set`, `unset` and `add into`), e.g. `set . { base_res 3440 1440 }`. |

Headers and keys compare without regard to case and to runs of spaces or tabs;
values compare exactly, quotes removed.

A selector must match exactly one block, or the patch fails (see
[When a patch fails](#when-a-patch-fails)). Write `set all <selector>` to
change every match on purpose.

### Operations

| Operation | Effect |
|---|---|
| `set <sel> { key value ... }` | Sets each key in the block: replaces the line if the key is there, adds it at the end of the block if not. Other lines are untouched. |
| `unset <sel> { key ... }` | Removes those keys from the block. |
| `add { blocks }` | Adds whole blocks at the end of the file. |
| `add into <sel> { blocks }` | Adds blocks at the end of that block. |
| `add before <sel> { blocks }` / `add after <sel> { blocks }` | Adds blocks next to that block (resource lists, where order matters). |
| `replace <sel> { block }` | Replaces the whole block. Conflicts with any other change to it; prefer `set`. |
| `remove <sel>` | Removes the block. |

For every file it patches, the loader starts from the pack's copy if the
pack has one, else the game's (loose, or from the `.pak` it's packed in),
applies every loaded mod's patches in load order, and serves the result.
Everything a patch doesn't touch is kept byte for byte: comments, tabs, line
endings, encoding. Pack updates to the file are picked up because the result
is rebuilt from the current copy at each launch.

Counted lists (*to verify*): `.sanim` files state how many objects they hold
(`total_elements 18`, `objects 18`). When patches add or remove objects the
loader rewrites those counts, so two mods adding objects don't both have to
change them.

### Scripts

A mod's Lua scripts and texture lists must be registered in
`data/ui/uiresources.txt` to load. Don't ship a copy of it: use `scripts` in
`mod.json`, which becomes this patch:

```
patch "data/ui/uiresources.txt"
{
	add after Item[fileName="ui\scripts\windows\equipmentwnd.lua"]
	{
		Item
		{
			fileName "ui\mods\retro-layout\layout.lua"
			resourceType "SCRIPT"
			loadingGroups "APP_START"
		}
	}
}
```

Without `after`, a script goes after the last script of its loading group.
`"type": "TEXTURE"` registers a texture list (`textures/*.txt`) the same way.

Place a script **after the game script whose functions it changes**, so they
exist when your script runs. That is the whole trick behind Lua hooks.

### When a patch fails

A patch fails when a selector matches nothing, or more than one block without
`all`. Then the whole `.sfpatch` file is left out (its other operations too,
so a mod is never half applied from one file), the rest of the mod still
loads, and the Mods page and `agent.log` say which operation and why. This is
usually a mod made for another game build, or another mod that removed or
replaced the block.

## Lua hooks

The UI is written in Lua (`data/ui/scripts`). A window class is a plain table
that a script fills with functions:

```lua
UI.HudModulesWnd = {}
function UI.HudModulesWnd:UpdateSpecialModule( module, cooldownVal ) ... end
```

A mod changes single functions of these tables instead of shipping the
script. The loader registers its own script, `sfmod.lua`, first in
`uiresources.txt`; it gives every mod the `SF` table:

```lua
-- runs after hudmoduleswnd.lua (mod.json: "after": "ui\\scripts\\windows\\hudmoduleswnd.lua")
local M = SF.mod( "cooldown-clock" )   -- this mod's handle; also creates UI.CooldownClock settings table

-- New function: no other mod can conflict with it as long as the name is yours.
function UI.HudModulesWnd:CooldownClock_Update( slotWnd, slotData, module, left, total )
	...
end

-- Wrap: run code around the original. Any number of mods can wrap the same function.
M:wrap( UI.HudModulesWnd, "UpdateSpecialModule", function( orig, self, module, cooldownVal, ... )
	local r = orig( self, module, cooldownVal, ... )
	self:CooldownClock_Update( self.specModuleSlot, self.specModuleData, module, cooldownVal )
	return r
end )

-- Replace: your function instead of the original. Only one mod's replacement can win.
M:replace( UI.EquipmentWnd, "LayoutPassiveShipSlots", function( self, ... )
	...
end )
```

| Call | Meaning |
|---|---|
| `SF.mod(id)` | Handle for your mod: `wrap`, `replace`, `log`, and `settings` (the table `UI.<ModName>` your players change from the console). |
| `M:wrap(tbl, name, fn)` | `fn(orig, ...)` is called instead of `tbl[name]`; call `orig` to run what was there. |
| `M:replace(tbl, name, fn)` | `fn` is the new base function. Wraps from other mods still run around it. |
| `M:log(fmt, ...)` | `sys.log` with your mod id in front. |
| `SF.version` | API version (`"1"`), for `requires.sfmod`. |

How they combine, whatever the load order:

- Each function has one **base** (the game's, or the last replacement) and a
  list of **wraps** in load order. `SF` rebuilds the function from those every
  time one is added, so a replacement registered after a wrap doesn't drop
  the wrap.
- Two mods **replacing** the same function is a conflict: the later one wins,
  and `SF` logs it (`[sfmod] conflict: UI.EquipmentWnd:LayoutPassiveShipSlots
  replaced by retro-layout and other-mod; other-mod wins`). The launcher shows
  these on the Mods page after the game exits, and warns before launch when
  two mods declare the same function in `lua.replaces`.
- Functions are stored back into the class table itself (no metatable
  tricks), so the game's native code sees them like the originals.

Use `wrap` whenever the change can run before or after the original. Use
`replace` only when it has to change something in the middle of a function,
and copy the original function as the starting point.

*To verify:* that the game doesn't keep its own references to class functions
between running a script and running the next one, and what happens to
hooks when the UI scripts are reloaded (the scripts re-run in the same order,
so the hooks should be applied again).

## New files

Put a mod's own files under `files/`, laid out like the game folder, at a
path named after the mod: `files/data/ui/mods/<id>/...` (in game paths,
`ui\mods\<id>\...`). New paths can't conflict with anything. Point the game
at them with a data patch: a texture list entry naming your `.dds`, a script
in `scripts`.

## Replacing game files

A file in `files/` at a game path replaces that game file outright. It's the
only way to change binary files (`.dds` textures, models, sounds) and it's
what texture packs do. Two mods replacing the same file is a conflict: the
later one wins and the Mods page shows both. A file inside a `.pak` can only
be replaced with the `loose-before-packed` core patch in `needs`.

A mod may not replace a file the pack ships unless the pack says so; that
file belongs to the server's setup.

## Text diffs

For a text file the data patch format can't express, a mod can ship a unified
diff (`patches/*.diff`, made with `diff -u` against the stock file). Diffs from
different mods apply one after another; they only conflict where they change
the same lines, and then the later one fails and is reported like a failed
patch. A diff only fits the game build it was made against, so a mod with
diffs should set `game`.

## Conflicts and load order

| Two mods both... | Result |
|---|---|
| Add new files, new functions, new blocks | No conflict. |
| Wrap the same function | No conflict. Both run, in load order (the later one outermost). |
| `set` different keys of the same block | No conflict. |
| `set` the same key of the same block | Later mod wins; reported. |
| Replace the same function | Later mod wins; reported. |
| Replace the same whole file | Later mod wins; reported. |
| One removes or `replace`s a block the other patches | The other's patch fails; reported. |

The load order is the player's `order`, then `after`/`before` hints, then id.
The loader is deterministic: the same mods in the same order always give the
same result. `agent.log` lists the loaded mods with versions and a hash of
the list, which is worth including in bug reports.

## Mods and the server

Client mods can't make the client and server disagree, because:

- The server owns everything with an id that goes over the network (ships,
  modules, items, maps). The client only looks up how to show an id the
  server sends.
- The client's UI data is keyed by names (`Texture name`, script paths,
  `UI.<Class>:<function>`), never by numbers handed out at load time. Two
  players loading the same mods in a different order get the same names;
  at most a conflict resolves differently on each screen.

To keep it that way:

- Name what you add after your mod id (`UI.HudModulesWnd:CooldownClock_Update`,
  `ui\mods\cooldown-clock\...`, `UI.CooldownClock`), never with a number or a
  list position. Don't save list positions to settings; save names.
- Don't rely on load order for correctness; it only decides who wins a
  reported conflict.
- A change the server must also have (new content, changed game data under
  `data/gamedata/`) is not a player mod. Set `"server": true`; it then loads
  only when the server's pack requires it.

*To verify:* whether the client reports hashes of any data files to the
server. Replaced files under `data/ui/` are known to be fine (packs and mods
already do it online).

## Rules for mod authors

1. Don't ship a copy of a game file; use a data patch, a Lua hook or a new file.
2. Prefer `set` to `replace` in data patches, and `wrap` to `replace` in Lua.
3. Name everything after your mod id: Lua globals under `UI.<ModName>`, new
   functions `<ModName>_...`, files under `ui\mods\<id>\`.
4. Register scripts and texture lists through `mod.json` or patches, never
   with a copy of `uiresources.txt`.
5. One feature per mod. A retro layout, retro icons and a cooldown clock are
   three mods a player can mix, not one.
6. Put settings in your `UI.<ModName>` table, and let players turn the mod's
   effect off from it (`UI.DeckGlyphs.enabled = false`).
7. Log through `M:log`; the first lines a player sends you will be from
   `game.log`.

## Converting the existing mods

- **sc-deck-glyphs** registers its script and atlas with copies of the 12
  per-language `uiresources.txt` files and keeps its wrappers in a metatable
  on `UI`, because it loads before `uigamefuncs.lua`. As a mod it registers
  with `scripts` (after `uigamefuncs.lua`) and uses `M:wrap` on
  `GetBindKeysStr`/`KeyName2Str`; the class `def` changes stay as they are.
- **coms-retro-sc-ui** ships 18,600 lines to change about 500:
  `module_new.txt` icon remaps become `set` patches, `equip_main2.sanim`
  becomes `set` patches on its objects, and the changes to
  `equipmentwnd.lua`, `hudmoduleswnd.lua` and `moduleswnd.lua` (about 17
  existing functions and 6 new ones) become one hook script per window. Split
  into `retro-icons`, `retro-layout` and `cooldown-clock`.

## Open questions (to verify in the game)

- Does a texture list registered later override a `Texture name` defined
  earlier? If so, icon mods could add their own list instead of patching.
- `.sanim` counts: are `total_elements` and `objects N` checked, and must
  they match?
- When are class `def` tables read: when the script runs, or when a window is
  first created? (Decides whether hooking `def` after the script is enough.)
- Do UI script reloads re-run `uiresources.txt` in order?
- Does the client send data file hashes to the server?
