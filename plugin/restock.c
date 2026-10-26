/* restock: a bbhost plugin that refills blood vials and quicksilver bullets
 * from storage when the player uses a Bold Hunter's Mark, the way the game
 * already does on death, and also the consumables the game never refills
 * (papers, molotovs, knives, ...: EXTRAS).
 *
 * It never creates items: everything goes through the game's own
 * storage-to-inventory transfer, which moves only what storage holds, up to
 * the carry cap.
 *
 * The engine addresses are the #defines below; docs/findings.md describes
 * what each function does and how that was established.
 *
 * Death and the Hunter's Dream: the game calls its restock (RESTOCK) then;
 * a hook on its entry runs the extras too, so they follow the game's own
 * rules for when to restock.
 *
 * Bold Hunter's Mark: on each world load (a warp, a death, a lamp), if the
 * inventory holds fewer marks than at the last load, one was used, so it
 * calls the restock. It also logs every goods item used up between loads,
 * which is how BOLD_HUNTERS_MARK was found. Only the Bold mark: the plain
 * Hunter's Mark (which forfeits your echoes) is left alone on purpose.
 *
 * All addresses are Binary Ninja addresses of the 1.09 eboot (image base
 * 0x400000).
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
static int extras = 1;
static int notify; /* show what moved on screen: off by default, for testing */
static int in_restock; /* restock() is calling RESTOCK: the hook leaves the extras to it */

/* docs/findings.md. */
#define GAME_DATA_MAN 0x593b130u    /* GameDataMan*; the local player's data at +8 */
#define PLAYER_INVENTORY 0x328u     /* PlayerGameData + 0x328: the inventory */
#define PLAYER_STORAGE 0x5b0u       /* PlayerGameData + 0x5b0: Inventory* storage */
#define RESOLVE_ITEM 0x1e89070u     /* {u32 handle, u64 0, u32 item out} */
#define RESTOCK 0x18dace0u          /* restock_from_storage(void) */
#define CAN_MOVE 0x18d9a80u         /* (Inventory* dst, u32* item, int available) -> how many fit */
#define TRANSFER 0x18da750u         /* (int src_slot, Inventory* src, Inventory* dst, int n) */
#define GOODS 0x40000000u
/* The Bold Hunter's Mark goods id, from restock.log: the one item used up
 * between loads when a mark was used (39 -> 38 -> 37). */
#define BOLD_HUNTERS_MARK 1400u

/* Consumables the game's restock skips, restocked the same way. Goods ids
 * from bloodborne-wiki.com (Datamined Item IDs; the DLC molotovs from
 * Datamined Internal Names). Left out on purpose: the Bold Hunter's Mark,
 * the special bloods, Insight items, story items, coldbloods. */
static const struct {
    uint32_t id;
    const char* name;
} EXTRAS[] = {
    {1100, "Antidote"},
    {1101, "Sedative"},
    {1110, "Beast Blood Pellet"},
    {1120, "Blue Elixir"},
    {1200, "Molotov Cocktail"},
    {1201, "Delayed Molotov"},
    {1210, "Poison Knife"},
    {1220, "Oil Urn"},
    {1230, "Pebble"},
    {1240, "Throwing Knife"},
    {1250, "Rope Molotov Cocktail"},
    {1251, "Delayed Rope Molotov"},
    {1270, "Numbing Mist"},
    {1300, "Fire Paper"},
    {1320, "Bolt Paper"},
    {1330, "Bone Marrow Ash"},
    {1410, "Shining Coins"},
    {2030, "Lead Elixir"},
    {2090, "Shaman Bone Blade"},
};

static const char* extra_name(uint32_t id) {
    for (size_t i = 0; i < sizeof(EXTRAS) / sizeof(EXTRAS[0]); i++)
        if (id == (GOODS | EXTRAS[i].id)) return EXTRAS[i].name;
    return NULL;
}

static const char* item_name(uint32_t id) {
    if (id == (GOODS | 1000u)) return "Blood Vial";
    if (id == (GOODS | 900u)) return "Quicksilver Bullets";
    return extra_name(id);
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

/* Goods totals in an inventory-layout container: 16-byte entries {handle, ?,
 * qty at +8, ?}, the first *(+0x24) at *(+0x58), the rest at *(+0x48), highest
 * index at +0x88. */
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

/* The game's restock loop (RESTOCK) with EXTRAS as the filter instead of
 * isAutoReplenish: for each storage slot holding one, move what fits. */
static void restock_extras(uint8_t* storage, uint8_t* inv) {
    for (uint32_t i = 0; i <= *(const uint32_t*)(storage + 0x88) && *(const uint32_t*)(storage + 0x88) != 0xffffffffu; i++) {
        uint32_t first = *(const uint32_t*)(storage + 0x24);
        const uint8_t* e = i < first ? *(uint8_t* const*)(storage + 0x58) + i * 16 : *(uint8_t* const*)(storage + 0x48) + (i - first) * 16;
        if (!e || !*(const uint32_t*)e) continue;
        uint32_t item = resolve(*(const uint32_t*)e);
        if (!extra_name(item)) continue;
        BbCallRegs regs;
        memset(&regs, 0, sizeof(regs));
        regs.arg[0] = (uint64_t)inv;
        regs.arg[1] = (uint64_t)&item;
        regs.arg[2] = (uint64_t)(uint32_t)*(const int32_t*)(e + 8);
        if (api->call_guest(CAN_MOVE, &regs) != 0) continue;
        int32_t n = (int32_t)regs.ret;
        if (n <= 0) continue;
        memset(&regs, 0, sizeof(regs));
        regs.arg[0] = i;
        regs.arg[1] = (uint64_t)storage;
        regs.arg[2] = (uint64_t)inv;
        regs.arg[3] = (uint64_t)n;
        api->call_guest(TRANSFER, &regs);
    }
}

/* Calls the game's restock (and the extras') and logs what moved. `why` names the caller.
 * Returns the number of items moved, or -1 with no player or storage. */
static int restock(const char* why, int* bad) {
    static Snapshot inv0, inv1, st0, st1;
    uint8_t* p = player_data();
    uint8_t* storage = p ? *(uint8_t**)(p + PLAYER_STORAGE) : NULL;
    *bad = 0;
    if (!p || !storage) {
        if (out) fprintf(out, "%s: no player data (%p) or storage (%p)\n", why, (void*)p, (void*)storage);
        return -1;
    }
    uint8_t* inv = p + PLAYER_INVENTORY;
    snapshot(inv, &inv0);
    snapshot(storage, &st0);
    BbCallRegs regs;
    memset(&regs, 0, sizeof(regs));
    in_restock = 1;
    int rc = api->call_guest(RESTOCK, &regs);
    in_restock = 0;
    if (extras) restock_extras(storage, inv);
    snapshot(inv, &inv1);
    snapshot(storage, &st1);
    if (out) fprintf(out, "%s: call_guest(restock) = %d; %d/%d goods in inventory, %d/%d in storage (before/after)\n", why, rc, inv0.n, inv1.n, st0.n, st1.n);
    int moved;
    *bad = report(&inv0, &inv1, &st0, &st1, &moved);
    if (out) fflush(out);
    return moved;
}

/* Entry of the game's restock, as the game calls it (death, the Dream): run
 * the extras first; then the game's own restock runs as usual. */
static const uint8_t RESTOCK_PROLOGUE[17] = {
    0x55,                   /* push rbp */
    0x48, 0x89, 0xe5,       /* mov rbp, rsp */
    0x41, 0x57, 0x41, 0x56, /* push r15; push r14 */
    0x41, 0x55, 0x41, 0x54, /* push r13; push r12 */
    0x53,                   /* push rbx */
    0x48, 0x83, 0xec, 0x38, /* sub rsp, 0x38 */
};

static int on_game_restock(BbHookCtx* ctx, void* user) {
    static Snapshot inv0, inv1, st0, st1;
    (void)ctx;
    (void)user;
    if (in_restock || !extras) return 0;
    uint8_t* p = player_data();
    uint8_t* storage = p ? *(uint8_t**)(p + PLAYER_STORAGE) : NULL;
    if (!p || !storage) return 0;
    uint8_t* inv = p + PLAYER_INVENTORY;
    snapshot(inv, &inv0);
    snapshot(storage, &st0);
    restock_extras(storage, inv);
    snapshot(inv, &inv1);
    snapshot(storage, &st1);
    if (out) fprintf(out, "game restock (death, the Dream, ...): extras\n");
    int moved;
    report(&inv0, &inv1, &st0, &st1, &moved);
    show_moved();
    if (out) fflush(out);
    return 0;
}

/* The inventory's goods at the last world load, to spot a used mark. */
static Snapshot last_load;
static int have_last_load;

static void on_world_load(uint32_t block, void* user) {
    static Snapshot now;
    (void)user;
    uint8_t* p = player_data();
    if (!p) return;
    snapshot(p + PLAYER_INVENTORY, &now);
    int mark_used = 0;
    if (have_last_load) {
        for (int k = 0; k < last_load.n; k++) {
            uint32_t id = last_load.c[k].id;
            int32_t before = last_load.c[k].qty, after = qty_of(&now, id);
            if (after >= before) continue;
            if (out) fprintf(out, "load %08x: goods %u used since the last load (%d -> %d)\n", block, id & 0x0fffffffu, before, after);
            if (BOLD_HUNTERS_MARK && id == (GOODS | BOLD_HUNTERS_MARK)) mark_used = 1;
        }
    }
    if (mark_used && on_mark) {
        int bad;
        if (restock("bold hunter's mark", &bad) > 0) show_moved();
        snapshot(p + PLAYER_INVENTORY, &now);
    }
    last_load = now;
    have_last_load = 1;
    if (out) fflush(out);
}

static void on_option(const char* key, const char* value, void* user) {
    (void)user;
    if (strcmp(key, "on_mark") == 0) on_mark = value && strcmp(value, "true") == 0;
    if (strcmp(key, "extras") == 0) extras = value && strcmp(value, "true") == 0;
    if (strcmp(key, "notify") == 0) notify = value && strcmp(value, "true") == 0;
    if (out) fprintf(out, "option %s = %s\n", key, value ? value : "");
}

/* Opt-in and gameplay: it changes the player's items, so bbhost puts it in
 * the session's ruleset. How that affects summons and invasions is an open
 * question (docs/findings.md). */
BB_PLUGIN_EXPORT const BbPluginInfo bb_plugin_info = {
    BB_PLUGIN_API_VERSION, BB_PLUGIN_OPT_IN | BB_PLUGIN_GAMEPLAY, "restock", "Restock", "0.0.1", "ambientmf",
    "Refills vials, bullets and other consumables from storage: on a Bold Hunter's Mark, and the others whenever the game restocks vials (death, the Dream)."};

BB_PLUGIN_EXPORT const BbOption bb_plugin_options[] = {
    {BB_OPT_BOOL | BB_OPT_LIVE, "on_mark", "Restock on Bold Hunter's Mark",
     "refill vials and bullets from storage after the mark warps you", "true", 0, 0, 0},
    {BB_OPT_BOOL | BB_OPT_LIVE, "extras", "Also other consumables",
     "papers, molotovs, knives, pellets, Blue Elixir, Numbing Mist, Shining Coins, ...: on a mark, a death and in the Dream", "true", 0, 0, 0},
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
    extras = strcmp(api->config("restock.extras"), "false") != 0;
    notify = strcmp(api->config("restock.notify"), "true") == 0;
    if (out) fprintf(out, "== start: on_mark=%d extras=%d notify=%d\n", on_mark, extras, notify);
    api->on_option("restock", on_option, NULL);
    api->on_world_load(on_world_load, NULL);
    if (api->hook2(RESTOCK, RESTOCK_PROLOGUE, sizeof(RESTOCK_PROLOGUE), on_game_restock, NULL) != 0) {
        api->log("restock: could not hook the game's restock; extras only on a Bold Hunter's Mark");
        if (out) fprintf(out, "hook on %x failed\n", RESTOCK);
    }
    api->log("restock: loaded");
    return 0;
}
