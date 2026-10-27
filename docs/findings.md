# Findings

How the game restocks from storage, and where the plugin steps in. The engine addresses the plugin needs are the `#define`s in `plugin/restock.c` and aren't repeated here. Engine structures that bbhost already documents (the game data manager and player data, the inventory container layout, `EquipGameData`, param rows and their fields) are in bbhost's `include/bbhost/` and its `boss_rush` plugin, and are only referenced here, not restated.

## What the plugin does

1. **The extra consumables** (listed below) are marked `isAutoReplenish` in `EquipParamGoods`, through bbhost's `param_set`. The game's own restock then refills them along with vials and bullets, wherever it runs: on death and in the Hunter's Dream (the game's own triggers), and when the plugin calls it.
2. **Bold Hunter's Mark:** when you use one, the plugin calls the game's restock, exactly as a death does.
3. **Home from another world:** when you come back from another player's world as a cooperator or invader, the plugin calls the game's restock. (Tested as a cooperator and as an invader.)

It does nothing else. Nothing is restocked at any other time, and the plain Hunter's Mark (which forfeits echoes) is ignored. There's no button to restock on demand: an early test button was removed because it would be easy to abuse.

## Rule: move items, never create them

The plugin only **moves** consumables the player already owns, from storage into the inventory, up to the normal carry cap. It never creates items:

- Nothing in storage means nothing happens. If storage holds fewer than the inventory is missing, only what's there moves.
- It saves the trip that otherwise costs a death or a run to the Hunter's Dream. Players still buy or farm their stock as usual.
- This holds by construction. The plugin never writes an item count. Every move goes through the game's own storage transfer, which refuses to move more than the source holds and subtracts from storage exactly what it adds to the inventory.
- Each restock logs every item count that changed, in both containers, and flags any item where storage didn't drop by exactly what the inventory gained (`restock.log`). In every test so far, storage lost exactly what the inventory gained.

## The game's restock

Only one routine in the game reads `EquipParamGoods.isAutoReplenish`, and it is the restock. It takes no arguments. It walks the player's storage and moves each auto-replenish item (vials and bullets) into the inventory, using two helpers: one works out how many fit, the other does the move.

```c
/* The game's restock. */
void restock_from_storage(void) {
    Inventory* storage = player_storage();
    Inventory* inv = player_inventory();
    for (uint32_t slot = 0; slot <= last_slot(storage); slot++) {
        uint32_t item = resolve_item(entry(storage, slot)->handle);
        const EQUIP_PARAM_GOODS_ST* row = goods_row(item);
        if (!row || !row->isAutoReplenish) continue;
        int n = how_many_fit(inv, &item, entry(storage, slot)->qty);   /* carry cap, at most what storage holds */
        storage_transfer(slot, storage, inv, n);
    }
}

/* Moves n of the item in src's slot into dst. Moves nothing and returns false
 * when the slot holds fewer than n. Otherwise adds n to dst, subtracts n from
 * src, and frees the slot if it empties. */
bool storage_transfer(int src_slot, Inventory* src, Inventory* dst, int n);
```

The game calls the restock from one place: the routine that places the player in a map. It runs there when the map is the Hunter's Dream, or when some other conditions hold that we haven't identified (presumably "after a death").

### Not ours: the supply-item fill

A second routine walks the inventory and tops up "supply items" (`isSuppleItem` / `isFullSuppleItem`) to a limit **without touching storage**, swapping some for their replacement item. It's reached through what looks like a script command. It creates items, so the plugin must never use it.

## The extras: one param flag

The game's restock refills whatever is flagged `isAutoReplenish`, so the plugin flags the extras and lets the game do the rest: storage counts, carry caps and the move itself are all the game's own. The plugin sets the flag when `EquipParamGoods` has loaded, again on every world load (in case the game reloaded the table), and puts back the game's own values when the `extras` option is turned off.

An earlier version ran its own copy of the game's loop, filtered by the plugin's item list, from a hook on the restock's entry. Setting the flag does the same with no hook and two fewer engine functions (the how-many-fit helper and the storage transfer).

## Detecting a return from another world

bbhost's `chr_list` gives the main player's multiplayer class (`chr_type`): 0 in your own world, another value as a phantom in someone else's. Going to another player's world is itself a world load, so the plugin reads it at each load and logs it. A load at 0 right after a load as a phantom is a return home, and the plugin restocks. Nothing runs between loads. The class is already set when bbhost reports the load into the host's world: the log showed 1 as a cooperator and 2 as an invader. The plugin doesn't depend on those values, only on "not 0, then 0".

## Detecting a Bold Hunter's Mark

Using a Bold Hunter's Mark applies a special effect to the player: its `EquipParamGoods` row (goods 1400) has `refCategory` 2 (special effect) and `refId` 2101. `SpEffectParam` 2101 is an instant effect with `stateInfo` 16, presumably the warp to the last lamp. The plain Hunter's Mark (goods 100) applies a different effect, 2100. Discarding or storing a mark applies no effect at all.

So the plugin hooks the game's effect insert-or-refresh, one of the apply-effect functions bbhost's SDK documents (`bbhost/engine/sprj/sp_effect.hpp`). The hook only notes a call carrying effect 2101, since the insert runs for every effect the game applies. On the next frame the plugin checks that the effect list it went to belongs to the main player. The restock then happens at the next world load: the warp the mark causes.

```c
/* The hook: runs for every effect inserted. */
int on_effect_insert(EffectList* list, uint32_t effect_id, ...) {
    if (effect_id == 2101) mark_effect_list = list;
    return 0;   /* let the game's insert run unchanged */
}

/* Next frame, on the main thread. */
if (mark_effect_list && owner(mark_effect_list) == main_player()) mark_used = 1;

/* Next world load. */
if (mark_used) { mark_used = 0; restock(); }
```

- **The Bold Hunter's Mark is goods 1400.** It was found from the plugin's log of items used up between loads: the only item that dropped by one per mark use (39 → 38 → 37). The wiki's item id table agrees.
- **Which function the effect takes:** the first test hooked all three apply-effect functions bbhost names (the character wrapper, eligibility-and-insert, insert-or-refresh) and logged which saw effect 2101. Only insert-or-refresh did, with the id as its second argument and the player's effect list as its first. The plugin now hooks only that one.
- **Why not count the marks:** an earlier version restocked when the inventory held fewer marks than at the last load. A discard (`isDrop` is set on the mark) also lowers the count, so discarding a mark and reloading restocked in place, skipping the warp. A mark picked up and then used between loads leaves the count unchanged, so that use was missed. The count is still logged as a cross-check.
- **Tested 2026-10-26:** two mark uses each logged the effect on the player and restocked (Blood Vial 19 → 20 with storage 170 → 169; Antidote and Sedative 9 → 10). Two discards followed by a reload lowered the count (29 → 28 → 27) with no effect and no restock. The plain Hunter's Mark didn't restock.
- **Retested with the single hook:** a mark use logged the effect on the player and restocked (Blood Vial, Sedative and Quicksilver Bullets +1 each, storage −1 each). A discard plus reload lowered the count (25 → 24) without a restock. Death and the Dream refilled through the game. A mark with nothing missing that storage could fill moved nothing.
- **A Hunter's Dream nuance:** a mark that lands in the Dream is restocked by the game itself, through the Dream case above. Restocking there too is harmless: nothing is left to move.
- **Not covered, on purpose:** the plain Hunter's Mark. It forfeits all held Blood Echoes (the Bold one keeps them).

## The extra consumables

The plugin restocks these (option `extras`, on by default) in addition to the vials and bullets the game handles. Goods ids are from bloodborne-wiki.com's [Datamined Item IDs](https://www.bloodborne-wiki.com/2018/01/datamined-item-ids.html). The DLC molotovs aren't on that page; their ids come from the same wiki's Datamined Internal Names page (時限爆発瓶 "Timed Explosion Bottle", 背後時限爆発瓶). The page's ids for Blood Vial (1000), Quicksilver Bullets (900) and Bold Hunter's Mark (1400) match what the plugin observed in game.

| id | item | id | item |
|---|---|---|---|
| 1100 | Antidote | 1250 | Rope Molotov Cocktail |
| 1101 | Sedative | 1251 | Delayed Rope Molotov (DLC) |
| 1110 | Beast Blood Pellet | 1270 | Numbing Mist |
| 1120 | Blue Elixir | 1300 | Fire Paper |
| 1200 | Molotov Cocktail | 1320 | Bolt Paper |
| 1201 | Delayed Molotov (DLC) | 1330 | Bone Marrow Ash |
| 1210 | Poison Knife | 1410 | Shining Coins |
| 1220 | Oil Urn | 2030 | Lead Elixir |
| 1230 | Pebble | 2090 | Shaman Bone Blade |
| 1240 | Throwing Knife | | |

Left out on purpose:
- **Bold Hunter's Mark:** it's the trigger, and restocking it would make it free.
- **Special bloods** (Blood of Adeline, Adella, Arianna, Iosefka's Blood Vial): one each.
- **Insight items** (Great One's Wisdom, Madman's Knowledge) and **story items** (Third Umbilical Cord): players move these in and out of storage deliberately.
- **Coldbloods:** echo items, not needed in combat.
- **Other misc items** (Blood Dreg, Pungent Blood Cocktail, Tear Stone, Vermin, ...): not standard combat stock.

## Open questions

- ~~**Online:** does the gameplay flag keep its players from matching with players who don't run it?~~ Yes. bbhost's docs (plugins, "Online rules") say a gameplay plugin is announced with every request (`X-BBHost-Ruleset`) and players only meet others with the same rules. With the flag, bbhost's log read `this session's online rules (X-BBHost-Ruleset): restock`.
- The plugin no longer sets the gameplay flag. It never creates items, and everything it moves the game would move on the next death or Dream visit, so it isn't announced and its players meet everyone. The flag is self-declared: bbhost reads it from the plugin and doesn't check it.
- ~~Does the game itself restock when a phantom returns home?~~ Not before the plugin's check: on both invader returns to a Chalice Dungeon, items were still short at the load, and the plugin's restock moved them. Coming home to the Hunter's Dream, the plugin found nothing short; that run can't tell whether the Dream's own restock ran first.
- The spawn routine's other conditions for restocking aren't identified; "after a death" is assumed, not yet confirmed.
- A routine that runs just before the restock at spawn looks up goods 901; not read yet.

## Lab notebook (2026-10-26)

### Finding the restock

Bounded static disassembly of the 1.09 eboot: stream scans for call targets and byte patterns, then small disassembly windows.

1. Scanning for direct byte tests of the `isAutoReplenish` bit found nothing: the compiler reads the goods param's bitfields as one wide integer and tests bits in that.
2. Callers of the carry-cap helper (the one `boss_rush` names) led to the supply-item fill above, which reads its flags that way. Searching for the same pattern on `isAutoReplenish`'s bit found exactly one reader: the restock.
3. The restock reads its two containers from the player data. The one it writes to is the inventory `boss_rush` uses; the one it reads from has the same layout, and its counts matched the in-game storage.

### Tests

The restock, called from a (since removed) test button, away from the Dream:

| test | vials (inventory, storage) | bullets (inventory, storage) |
|---|---|---|
| 17/17 held | 17 → 20, 206 → 203 | 17 → 20, 168 → 165 |
| all vials used | 0 → 20, 203 → 183 | unchanged |
| one bullet used | unchanged | 19 → 20, 165 → 164 |

The Bold Hunter's Mark trigger, in the Chalice Dungeons: vials 18 → 20 with storage 176 → 174, bullets 18 → 20 with storage 155 → 153; on the next mark, vials 19 → 20 with storage 174 → 173.

In every test, storage lost exactly what the inventory gained, the cap (20) held, and items already at the cap were left alone.

The extras, on three Bold Hunter's Mark uses in the Chalice Dungeons. Screenshots of the inventory menu before and after each mark are in [`screenshots/2026-10-26-extras/`](screenshots/2026-10-26-extras/):

| item | result |
|---|---|
| Antidote, Sedative | 9 → 10 on each mark; storage down one each time (19 → 16, 12 → 9) |
| Fire Paper | 9 → 10 with storage 1 → 0; after that it stayed short, with nothing in storage |
| Rope Molotov Cocktail | 9 → 10 twice, then 6 → 8 with storage 2 → 0: a partial refill, all there was |
| Shaman Bone Blade | 2 → 3 with storage 3 → 2: topped up to its cap of 3, though none was used |
| Numbing Mist | used each time (8 → 5), never refilled: none in storage |

No item was given for free. The tester checked this in game, and the log shows it: an item with nothing in storage was never refilled (Numbing Mist), an item stopped refilling once its storage ran out (Fire Paper), and a short supply moved only what was left (Rope Molotov). Storage lost exactly what the inventory gained for every item, each item's own cap held (10 for most, 3 for Shaman Bone Blade), and the menu matched the log (Rope Molotov: 6 held / 2 stored before, 8 / 0 after). Blood Echoes were unchanged across the marks.

Death and the Dream were later tested with the param flag (below), which replaced the plugin's own loop and hook.

### The param flag, as a data mod (2026-10-26)

To test whether the flag alone is enough, a bbhost param override (`<mods>/params/EquipParamGoods.toml`, `row.<id>.isAutoReplenish = 1` for the 19 extras) was used, with the plugin's own extras restock off:

- bbhost applied it on top of a whole-file param mod that was also installed: `EquipParamGoods.toml applied, 19 fields over 19 rows`.
- Death: Antidote 7 → 10, storage 16 → 13. Hunter's Dream: Antidote 9 → 10, storage 13 → 12. Bold Hunter's Mark (the plugin calling the game's restock): Sedative 9 → 10, storage 9 → 8.
- An item with nothing in storage stayed short.
- The session's online rules listed only the plugin: **bbhost doesn't announce param overrides online.** The plugin sets the same flag at runtime, and is not announced either (see Open questions).

### Return home, online (2026-10-26)

With the gameplay flag dropped, bbhost's log read `this session's online rules (X-BBHost-Ruleset): vanilla`, and the tester was summoned and invaded with players who don't run Restock.

As a cooperator in a host's Chalice Dungeon, summoned from the Hunter's Dream: the log read `chr_type 1` in the host's world, then `chr_type 0, home from another world` back in the Dream. The plugin's restock moved nothing; nothing was short.

As an invader (the second invader in a host's modded Chalice Dungeon, with bbhost's five-players enhancement on), coming home to the tester's own Chalice Dungeon, twice:

| item | first return (inventory, storage) | second return (inventory, storage) |
|---|---|---|
| Blood Vial | 18 → 20, 495 → 493 | 18 → 20, 493 → 491 |
| Quicksilver Bullets | 18 → 20, 395 → 393 | 18 → 20, 393 → 391 |
| Fire Paper | 9 → 10, 93 → 92 | 9 → 10, 92 → 91 |
| Rope Molotov Cocktail | 9 → 10, 79 → 78 | 9 → 10, 78 → 77 |
| Pebble | 19 → 20, 78 → 77 | 19 → 20, 77 → 76 |
| Sedative | unchanged | 9 → 10, 14 → 13 |

The log read `chr_type 2` in the host's world each time. Each return restocked once, storage lost exactly what the inventory gained, and the caps held. Screenshots of the second return, from the host's death ("It is time to bid farewell") to the refill at home, are in [`screenshots/2026-10-26-return/`](screenshots/2026-10-26-return/): vials and bullets 18 before, 20 after, Pebble 19 → 20, and the restock message listing the same items as the log.

### With bbhost's rebirth enhancement (2026-10-26)

Asked by a player: does Restock clash with bbhost's rebirth (respec at the Altar of Despair)? In one session in the Altar's map, in this order: a Bold Hunter's Mark restock (Blood Vial 19 → 20, Beast Blood Pellet 9 → 10, Bolt Paper 9 → 10, Quicksilver Bullets 16 → 20, storage down by the same), a rebirth from level 120 to 10 (bbhost's log: `3500130 blood echoes given back`), a second rebirth (10 to 10), then another mark restock (Blood Vial 19 → 20, Bolt Paper 8 → 10, Quicksilver Bullets 19 → 20). The rebirth kept its effect, as the tester confirmed in game, and both restocks behaved as before. Screenshots in [`screenshots/2026-10-26-rebirth/`](screenshots/2026-10-26-rebirth/): the level-up menu rebuilding the character from level 10 after the rebirth, the second Bold Hunter's Mark ("Do you wish to reawaken?", 86 marks, vials and bullets at 19), its restock message (Blood Vial +1, Bolt Paper +2, Quicksilver Bullets +1), and the stats after the mark: the rebuilt allocation (Vitality 48, Bloodtinge 42, Arcane 6) still in place and Blood Echoes unchanged by the mark (34612519).

## Plan from here

1. Read the goods-901 routine at spawn, and the storage transfer's other callers (likely the storage menu).
