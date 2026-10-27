# Findings

How the game restocks from storage, and where the plugin steps in. The engine addresses the plugin needs are the `#define`s in `plugin/restock.c` and aren't repeated here. Engine structures that bbhost already documents (the game data manager and player data, the inventory container layout, `EquipGameData`, param rows and their fields) are in bbhost's `include/bbhost/` and its `boss_rush` plugin, and are only referenced here, not restated.

## What the plugin does

1. **Bold Hunter's Mark:** when you use one, it calls the game's own restock, which refills Blood Vials and Quicksilver Bullets from storage exactly as a death does. Then it does the same for the extra consumables listed below.
2. **Death and the Hunter's Dream:** the game already calls its restock there, for vials and bullets only. A hook on the restock's entry also restocks the extra consumables, so they follow the game's own rules for *when* to restock.

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

## The plugin's extras restock

The same loop as the game's, with the plugin's item list in place of the param flag, through the same two helpers:

```c
void restock_extras(Inventory* storage, Inventory* inv) {
    for (uint32_t slot = 0; slot <= last_slot(storage); slot++) {
        uint32_t item = resolve_item(entry(storage, slot)->handle);
        if (!in_extras_list(item)) continue;   /* the only difference from the game's loop */
        int n = how_many_fit(inv, &item, entry(storage, slot)->qty);
        if (n > 0) storage_transfer(slot, storage, inv, n);
    }
}
```

The hook sits on the game's restock entry, runs `restock_extras`, and returns without skipping, so the game's restock then runs unchanged. When the plugin itself calls the restock (after a Bold Hunter's Mark), a flag tells the hook to stand aside, and the plugin runs the extras once afterwards.

## Detecting a Bold Hunter's Mark

No hook needed. bbhost's `on_world_load` fires on every placement in a map (warp, death, lamp, load). The plugin keeps the inventory's goods counts from the last load. If the Bold Hunter's Mark count dropped, a mark was used, and it restocks.

- **The Bold Hunter's Mark is goods 1400.** The plugin's "used since the last load" log found it: the only item that dropped by one per mark use (39 → 38 → 37). The wiki's item id table agrees.
- **A Hunter's Dream nuance:** a mark that lands in the Dream is restocked by the game itself, through the Dream case above. Only marks that land elsewhere need the plugin. Restocking there too is harmless: nothing is left to move.
- **Not covered, on purpose:** the plain Hunter's Mark. It forfeits all held Blood Echoes (the Bold one keeps them).
- Switching characters can compare against the previous character's baseline and restock once. That still only moves the new character's own storage.

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

- **Online:** the plugin is flagged `BB_PLUGIN_GAMEPLAY`, so bbhost adds it to the session's ruleset. Does that keep its players from matching (summons, invasions) with players who don't run it? Untested: no one was online to match with.
- Does the game also restock when a cooperator or invader returns to their own world? If so, the hook covers that case too. Untested, same reason.
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

Built, not yet tested: the extras (each item's cap), and the hook on death and in the Dream (expect a `game restock (death, the Dream, ...): extras` block in `restock.log`).

## Plan from here

1. Test the extras and the death/Dream hook in game.
2. Online: cooperator/invader return, and matchmaking with the plugin on.
3. Read the goods-901 routine at spawn, and the storage transfer's other callers (likely the storage menu).
