/* restock: a bbhost plugin that refills consumables from storage when the
 * player uses a Bold Hunter's Mark or comes home from another player's world,
 * the way the game already does on death and in the Hunter's Dream.
 *
 * Two parts:
 *
 * - The extras (papers, molotovs, knives, ...: EXTRAS) are marked
 *   EquipParamGoods.isAutoReplenish, through bbhost's param_set. The game's
 *   own restock (RESTOCK) then refills them along with vials and bullets,
 *   wherever it runs: on death, in the Dream, and when this plugin calls it.
 * - Triggers the game lacks: on each world load, if the inventory holds fewer
 *   Bold Hunter's Marks than at the last load, one was used; if the player was
 *   a phantom (chr_type != 0) at the last load and is home again (0), they came
 *   back from another world. Either way the plugin calls RESTOCK.
 *
 * It never creates items: the game's restock moves only what storage holds,
 * up to each item's carry cap. Only the Bold mark: the plain Hunter's Mark
 * (which forfeits your echoes) is left alone on purpose.
 *
 * The engine addresses are the #defines below (Binary Ninja addresses of the
 * 1.09 eboot, image base 0x400000); docs/findings.md describes what each
 * function does and how that was established.
 *
 * Copyright (C) 2026 ambientmf
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version. This program is distributed WITHOUT ANY WARRANTY; see
 * the GNU General Public License (LICENSE) for details. */
#include "bbhost_plugin.h"

#include <stdio.h>
#include <string.h>

static const BbHostApi* api;
static FILE* out;
static int on_mark = 1;
static int on_return = 1;
static int extras = 1;
static int notify; /* show what moved on screen: off by default, for testing */

#define GAME_DATA_MAN 0x593b130u    /* GameDataMan*; the local player's data at +8 */
#define PLAYER_INVENTORY 0x328u     /* PlayerGameData + 0x328: the inventory */
#define PLAYER_STORAGE 0x5b0u       /* PlayerGameData + 0x5b0: Inventory* storage */
#define RESOLVE_ITEM 0x1e89070u     /* {u32 handle, u64 0, u32 item out} */
#define RESTOCK 0x18dace0u          /* restock_from_storage(void) */
#define GOODS 0x40000000u
/* The Bold Hunter's Mark goods id, from restock.log: the one item used up
 * between loads when a mark was used (39 -> 38 -> 37). */
#define BOLD_HUNTERS_MARK 1400u

/* Consumables the game's restock skips unless marked auto-replenish. Goods
 * ids from bloodborne-wiki.com (Datamined Item IDs; the DLC molotovs from
 * Datamined Internal Names). Left out on purpose: the Bold Hunter's Mark,
 * the special bloods, Insight items, story items, coldbloods. */
static struct {
    uint32_t id;
    const char* name;
    char orig[8]; /* isAutoReplenish as the game had it; "" until read */
} EXTRAS[] = {
    {1100, "Antidote", ""},
    {1101, "Sedative", ""},
    {1110, "Beast Blood Pellet", ""},
    {1120, "Blue Elixir", ""},
    {1200, "Molotov Cocktail", ""},
    {1201, "Delayed Molotov", ""},
    {1210, "Poison Knife", ""},
    {1220, "Oil Urn", ""},
    {1230, "Pebble", ""},
    {1240, "Throwing Knife", ""},
    {1250, "Rope Molotov Cocktail", ""},
    {1251, "Delayed Rope Molotov", ""},
    {1270, "Numbing Mist", ""},
    {1300, "Fire Paper", ""},
    {1320, "Bolt Paper", ""},
    {1330, "Bone Marrow Ash", ""},
    {1410, "Shining Coins", ""},
    {2030, "Lead Elixir", ""},
    {2090, "Shaman Bone Blade", ""},
};
#define N_EXTRAS (sizeof(EXTRAS) / sizeof(EXTRAS[0]))

/* Marks the extras auto-replenish (on) or puts back the game's own values
 * (off). Returns 0 until EquipParamGoods has loaded. Called again on every
 * world load, in case the game reloaded the table. */
static int flags_set;

static int apply_flags(int on) {
    int changed = 0;
    for (size_t i = 0; i < N_EXTRAS; i++) {
        if (!EXTRAS[i].orig[0] && api->param_get("EquipParamGoods", EXTRAS[i].id, "isAutoReplenish", EXTRAS[i].orig, sizeof(EXTRAS[i].orig)) != 0) {
            EXTRAS[i].orig[0] = 0;
            return 0;
        }
        if (api->param_set("EquipParamGoods", EXTRAS[i].id, "isAutoReplenish", on ? "1" : EXTRAS[i].orig) == 0) changed++;
    }
    if (out && !flags_set) fprintf(out, "extras %s: isAutoReplenish set on %d of %d rows\n", on ? "on" : "off", changed, (int)N_EXTRAS);
    return 1;
}

static const char* item_name(uint32_t id) {
    if (id == (GOODS | 1000u)) return "Blood Vial";
    if (id == (GOODS | 900u)) return "Quicksilver Bullets";
    for (size_t i = 0; i < N_EXTRAS; i++)
        if (id == (GOODS | EXTRAS[i].id)) return EXTRAS[i].name;
    return NULL;
}

/* What a restock moved, for the on-screen message: "Blood Vial +1, ...". */
static char moved_text[256];

static void note_moved(uint32_t id, int32_t n) {
    char part[64];
    const char* name = item_name(id);
    if (name)
        snprintf(part, sizeof(part), "%s%s +%d", moved_text[0] ? ", " : "", name, n);
    else
        snprintf(part, sizeof(part), "%sgoods %u +%d", moved_text[0] ? ", " : "", id & 0x0fffffffu, n);
    if (strlen(moved_text) + strlen(part) < sizeof(moved_text) - 4)
        strcat(moved_text, part);
    else if (!strstr(moved_text, "..."))
        strcat(moved_text, ", ...");
}

static void show_moved(void) {
    char msg[300];
    if (!notify || !moved_text[0]) return;
    snprintf(msg, sizeof(msg), "Restocked from storage: %s", moved_text);
    api->show_message(msg, 5.0f);
}

typedef struct {
    uint32_t id; /* resolved item id, category bits included */
    int32_t qty;
} Count;
typedef struct {
    Count c[1024];
    int n;
} Snapshot;

static uint8_t* player_data(void) {
    uint8_t** man = (uint8_t**)api->guest_addr(GAME_DATA_MAN);
    return man && *man ? *(uint8_t**)(*man + 8) : NULL;
}

static uint32_t resolve(uint32_t handle) {
    struct {
        uint32_t handle, pad;
        uint64_t zero;
        uint32_t item, pad2;
    } r = {handle, 0, 0, 0xffffffffu, 0};
    BbCallRegs regs;
    memset(&regs, 0, sizeof(regs));
    regs.arg[0] = (uint64_t)&r;
    return api->call_guest(RESOLVE_ITEM, &regs) == 0 ? r.item : 0xffffffffu;
}

/* Goods totals in an inventory-layout container (bbhost's boss_rush documents
 * the layout): 16-byte entries {handle, ?, qty at +8, ?}, the first *(+0x24)
 * at *(+0x58), the rest at *(+0x48), highest index at +0x88. */
static void snapshot(const uint8_t* c, Snapshot* s) {
    s->n = 0;
    if (!c) return;
    uint32_t last = *(const uint32_t*)(c + 0x88), first = *(const uint32_t*)(c + 0x24);
    if (last == 0xffffffffu) return;
    for (uint32_t i = 0; i <= last; i++) {
        const uint8_t* e = i < first ? *(uint8_t* const*)(c + 0x58) + i * 16 : *(uint8_t* const*)(c + 0x48) + (i - first) * 16;
        if (!e || !*(const uint32_t*)e) continue;
        uint32_t id = resolve(*(const uint32_t*)e);
        if ((id & 0xf0000000u) != GOODS) continue;
        int k = 0;
        while (k < s->n && s->c[k].id != id) k++;
        if (k == s->n) {
            if (s->n == (int)(sizeof(s->c) / sizeof(s->c[0]))) continue;
            s->c[s->n++] = (Count){id, 0};
        }
        s->c[k].qty += *(const int32_t*)(e + 8);
    }
}

static int32_t qty_of(const Snapshot* s, uint32_t id) {
    for (int k = 0; k < s->n; k++)
        if (s->c[k].id == id) return s->c[k].qty;
    return 0;
}

/* Logs every goods count that changed, and whether storage lost exactly
 * what the inventory gained. Returns the number of items that broke that. */
static int report(const Snapshot* inv0, const Snapshot* inv1, const Snapshot* st0, const Snapshot* st1, int* moved) {
    const Snapshot* all[4] = {inv0, inv1, st0, st1};
    uint32_t seen[4096];
    int nseen = 0, bad = 0;
    *moved = 0;
    moved_text[0] = 0;
    for (int a = 0; a < 4; a++)
        for (int k = 0; k < all[a]->n; k++) {
            uint32_t id = all[a]->c[k].id;
            int dup = 0;
            for (int j = 0; j < nseen && !dup; j++) dup = seen[j] == id;
            if (dup) continue;
            seen[nseen++] = id;
            int32_t di = qty_of(inv1, id) - qty_of(inv0, id), ds = qty_of(st1, id) - qty_of(st0, id);
            if (!di && !ds) continue;
            if (di != -ds) bad++;
            if (di > 0) {
                *moved += di;
                note_moved(id, di);
            }
            if (out) {
                const char* name = item_name(id);
                fprintf(out, "  goods %u%s%s: inventory %d -> %d, storage %d -> %d%s\n", id & 0x0fffffffu, name ? " " : "", name ? name : "",
                        qty_of(inv0, id), qty_of(inv1, id), qty_of(st0, id), qty_of(st1, id), di != -ds ? "  ** MISMATCH **" : "");
            }
        }
    return bad;
}

/* Calls the game's restock and logs what moved. `why` names the trigger. */
static void restock(const char* why) {
    static Snapshot inv0, inv1, st0, st1;
    uint8_t* p = player_data();
    uint8_t* storage = p ? *(uint8_t**)(p + PLAYER_STORAGE) : NULL;
    if (!p || !storage) {
        if (out) fprintf(out, "%s: no player data (%p) or storage (%p)\n", why, (void*)p, (void*)storage);
        return;
    }
    uint8_t* inv = p + PLAYER_INVENTORY;
    snapshot(inv, &inv0);
    snapshot(storage, &st0);
    BbCallRegs regs;
    memset(&regs, 0, sizeof(regs));
    int rc = api->call_guest(RESTOCK, &regs);
    snapshot(inv, &inv1);
    snapshot(storage, &st1);
    if (out) fprintf(out, "%s: call_guest(restock) = %d; %d/%d goods in inventory, %d/%d in storage (before/after)\n", why, rc, inv0.n, inv1.n, st0.n, st1.n);
    int moved;
    report(&inv0, &inv1, &st0, &st1, &moved);
    if (moved > 0) show_moved();
    if (out) fflush(out);
}

/* The main player's multiplayer class: 0 at home, another value as a phantom
 * in someone else's world. -1 when there is no player. */
static int32_t player_chr_type(void) {
    BbChr chrs[64];
    size_t n = 0;
    if (api->chr_list(chrs, 64, &n) != 0) return -1;
    if (n > 64) n = 64;
    for (size_t i = 0; i < n; i++)
        if (chrs[i].is_player) return chrs[i].chr_type;
    return -1;
}

/* Sets the extras' flags once EquipParamGoods has loaded (it loads during the
 * boot), then stops doing anything. */
static void on_frame(void* user) {
    (void)user;
    if (!flags_set && extras) flags_set = apply_flags(1);
}

/* The inventory's goods and the player's chr_type at the last world load, to
 * spot a used mark and a return from another world. */
static Snapshot last_load;
static int have_last_load;
static int32_t last_chr_type = -1;

static void on_world_load(uint32_t block, void* user) {
    static Snapshot now;
    (void)user;
    if (extras) apply_flags(1);
    uint8_t* p = player_data();
    if (!p) return;
    snapshot(p + PLAYER_INVENTORY, &now);
    int mark_used = 0;
    if (have_last_load) {
        int32_t before = qty_of(&last_load, GOODS | BOLD_HUNTERS_MARK), after = qty_of(&now, GOODS | BOLD_HUNTERS_MARK);
        if (after < before) {
            mark_used = 1;
            if (out) fprintf(out, "load %08x: Bold Hunter's Mark used (%d -> %d)\n", block, before, after);
        }
    }
    int32_t t = player_chr_type();
    int returned = last_chr_type > 0 && t == 0;
    if (out) fprintf(out, "load %08x: chr_type %d%s\n", block, t, returned ? ", home from another world" : "");
    last_chr_type = t;
    if ((mark_used && on_mark) || (returned && on_return)) {
        restock(mark_used ? "bold hunter's mark" : "home from another world");
        snapshot(p + PLAYER_INVENTORY, &now);
    }
    last_load = now;
    have_last_load = 1;
    if (out) fflush(out);
}

static void on_option(const char* key, const char* value, void* user) {
    (void)user;
    int on = value && strcmp(value, "true") == 0;
    if (strcmp(key, "on_mark") == 0) on_mark = on;
    if (strcmp(key, "on_return") == 0) on_return = on;
    if (strcmp(key, "notify") == 0) notify = on;
    if (strcmp(key, "extras") == 0 && on != extras) {
        extras = on;
        flags_set = 0;
        flags_set = apply_flags(on);
    }
    if (out) fprintf(out, "option %s = %s\n", key, value ? value : ""), fflush(out);
}

/* Opt-in and gameplay: it changes the player's items, so bbhost puts it in
 * the session's online rules: players with it on only meet each other. */
BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, BB_PLUGIN_OPT_IN | BB_PLUGIN_GAMEPLAY, "restock", "Restock", "0.1.0", "ambientmf",
    "Refills vials, bullets and other consumables from storage after a Bold Hunter's Mark or a return from another world, and the other consumables also on death and in the Dream."};

BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_BOOL | BB_OPT_LIVE, "on_mark", "Restock on Bold Hunter's Mark",
     "refill from storage after the mark warps you", "true", 0, 0, 0},
    {BB_OPT_BOOL | BB_OPT_LIVE, "on_return", "Restock on return from another world",
     "refill from storage when you come home as a cooperator or invader", "true", 0, 0, 0},
    {BB_OPT_BOOL | BB_OPT_LIVE, "extras", "Also other consumables",
     "papers, molotovs, knives, pellets, Blue Elixir, Numbing Mist, Shining Coins, ...: whenever the game restocks vials", "true", 0, 0, 0},
    {BB_OPT_BOOL | BB_OPT_LIVE, "notify", "Show what was restocked",
     "a message listing what moved from storage (for testing; the game itself shows none)", "false", 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0}};

BB_PLUGIN_EXPORT int bb_plugin_image(const BbHostApi* host) {
    if (host->version < 9 || !host->eboot_is_109()) return 1;
    api = host;
    char path[1024];
    snprintf(path, sizeof(path), "%s/restock.log", api->plugin_dir("restock"));
    out = fopen(path, "a");
    on_mark = strcmp(api->config("restock.on_mark"), "false") != 0;
    on_return = strcmp(api->config("restock.on_return"), "false") != 0;
    extras = strcmp(api->config("restock.extras"), "false") != 0;
    notify = strcmp(api->config("restock.notify"), "true") == 0;
    if (out) fprintf(out, "== start 0.1.0: on_mark=%d on_return=%d extras=%d notify=%d\n", on_mark, on_return, extras, notify);
    api->on_option("restock", on_option, NULL);
    api->on_world_load(on_world_load, NULL);
    api->on_frame(on_frame, NULL);
    api->log("restock: loaded");
    return 0;
}
