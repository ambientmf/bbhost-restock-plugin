# Restock

A [bbhost](https://github.com/droogie/bbhost) plugin for Bloodborne on PC. It refills your consumables from storage, so you don't have to die or go back to the Hunter's Dream to restock.

- **Bold Hunter's Mark:** using one refills Blood Vials and Quicksilver Bullets from storage, exactly as a death does, plus the other consumables below.
- **Death and the Hunter's Dream:** the game already refills vials and bullets there. The plugin also refills the other consumables at those moments.

Other consumables (option "Also other consumables", on by default): Antidote, Sedative, Beast Blood Pellet, Blue Elixir, Molotov Cocktail, Delayed Molotov, Rope Molotov Cocktail, Delayed Rope Molotov, Poison Knife, Throwing Knife, Oil Urn, Pebble, Numbing Mist, Fire Paper, Bolt Paper, Bone Marrow Ash, Shining Coins, Lead Elixir, Shaman Bone Blade.

**It only moves what you already have.** Items come out of your storage, up to the normal carry limit, through the game's own storage transfer. It never adds items. With nothing in storage, nothing happens. Buy or farm your stock as usual. The plain Hunter's Mark (which costs your echoes), the Bold Hunter's Mark itself, special bloods, Insight items, story items and coldbloods are never touched.

How it works: [`docs/findings.md`](docs/findings.md).

**Status: in testing.** The Bold Hunter's Mark restock is tested, for vials, bullets and the other consumables. The death/Dream restock of the other consumables is built but untested. Online play (cooperators, invaders, matchmaking) is untested.

## Install

Not released yet. When it is:

1. Put `restock.dll` in the `plugins` folder next to the bbhost executable.
2. Turn it on in the launcher's Plugins tab or the in-game plugin menu (F9). It's opt-in, because it changes gameplay.

Requires Bloodborne 1.09 and bbhost with plugin API 9 or newer.

Options (bbhost menu, or `[restock]` in `bbhost.toml`): `on_mark` (restock on a Bold Hunter's Mark), `extras` (the other consumables) and `notify` (off by default: a message listing what moved, for testing; the game itself shows none). All take effect at once. The plugin logs every restock, item by item, to `plugins/restock/restock.log` in bbhost's data folder.

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
