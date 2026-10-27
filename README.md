# Restock

A [bbhost](https://github.com/droogie/bbhost) plugin for Bloodborne on PC. It refills your consumables from storage, so you don't have to die or go back to the Hunter's Dream to restock.

- **Bold Hunter's Mark:** using one refills Blood Vials, Quicksilver Bullets and the other consumables below from storage, exactly as a death does.
- **Home from another world:** coming back from someone else's world as a cooperator or invader does the same.
- **Death and the Hunter's Dream:** the game already refills vials and bullets there; with the plugin it refills the other consumables too.

Other consumables (option "Also other consumables", on by default): Antidote, Sedative, Beast Blood Pellet, Blue Elixir, Molotov Cocktail, Delayed Molotov, Rope Molotov Cocktail, Delayed Rope Molotov, Poison Knife, Throwing Knife, Oil Urn, Pebble, Numbing Mist, Fire Paper, Bolt Paper, Bone Marrow Ash, Shining Coins, Lead Elixir, Shaman Bone Blade.

**It only moves what you already have.** Items come out of your storage, up to the normal carry limit, through the game's own storage transfer. It never adds items. With nothing in storage, nothing happens. Buy or farm your stock as usual. The plain Hunter's Mark (which costs your echoes), the Bold Hunter's Mark itself, special bloods, Insight items, story items and coldbloods are never touched.

How it works: [`docs/findings.md`](docs/findings.md).

**Online:** Restock doesn't flag itself as a gameplay plugin, so bbhost leaves it out of your session's online rules and you meet players with or without it. It never creates items and only saves a trip, which a death or the Hunter's Dream gives anyone. bbhost's log shows the active rules at startup (`plugin: this session's online rules (X-BBHost-Ruleset)`); Restock isn't listed there. See bbhost's plugin docs, "Online rules".

**Status: in testing.** Tested in game: the Bold Hunter's Mark restock, the death and Dream restock of the other consumables, and coming home as a cooperator and as an invader, with matchmaking against players who don't run Restock.

## Install

Not released yet. When it is:

1. Put `restock.dll` in the `plugins` folder next to the bbhost executable.
2. Turn it on in the launcher's Plugins tab or the in-game plugin menu (F9). It's opt-in.

Requires Bloodborne 1.09 and bbhost with plugin API 9 or newer.

Options (bbhost menu, or `[restock]` in `bbhost.toml`): `on_mark` (restock on a Bold Hunter's Mark), `on_return` (on coming home from another world), `extras` (the other consumables) and `notify` (off by default: a message listing what moved, for testing; the game itself shows none). All take effect at once. The plugin logs every restock, item by item, to `plugins/restock/restock.log` in bbhost's data folder.

## License

GPL-3.0-or-later (see `LICENSE`), as bbhost requires of plugins built against its API.

## Development

```sh
tools/build-plugin.sh [path/to/bbhost]    # clang + mingw-w64, from WSL/Linux; uses the bbhost checkout's include/
cp build/restock.dll <bbhost folder>/plugins/
```

```
plugin/   the plugin (restock.c)
tools/    build script
docs/     research notes
```

Game files, saves and RE databases are gitignored; bring your own legally dumped copy of the game.
