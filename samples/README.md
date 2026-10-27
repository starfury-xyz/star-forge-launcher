# Sample server packs

Ready-made packs for community servers, kept here so new players can start
with one click (**Add profile → Community servers** in the launcher) and so
server teams have a working example to copy.

| Pack | Server | Game files |
|---|---|---|
| [`sc-classic-europe/`](sc-classic-europe/pack.json) | Star Conflict Europe (unofficial) | from the server's SCMOD1 list |
| [`starfury/`](starfury/pack.json) | STARFURY.XYZ:3801 | none; **retired** |

The launcher's STARFURY button now installs the server's own signed pack
(`https://starfury.web.garage.cosmosim.dev/pack.json`, id `xyz.starfury`).
`starfury/` stays so profiles installed from it keep their update address.

These are maintained by STAR FORGE, not by the server teams, and they are not
signed. Each one is a single `pack.json`: the launcher can install a pack from
a published `pack.json` alone when it has no files of its own, and these
check their copy here for updates (`update.url`).

When a server publishes its own signed pack, use that instead; its id will
differ (these use `org.starforge.samples.*`), so both can be installed side by
side while players move over.

To try one from a checkout: `sc-launch install samples/sc-classic-europe`.
