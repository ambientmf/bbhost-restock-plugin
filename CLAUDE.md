# Notes for Claude

A bbhost plugin (C, Windows DLL) for Bloodborne 1.09.

## Design rule

The plugin must never create items. It only moves consumables the player owns from storage into the inventory, up to the carry cap: vials and bullets (as the game's death restock does), and also consumables the game never refills (Numbing Mist, Fire/Bolt Paper, Shining Coins, Bone Marrow Ash, ...). Whatever it adds to the inventory it takes out of storage. With nothing in storage, nothing happens. It's a convenience to save the death or Hunter's Dream trip, not a cheat. Details: `docs/findings.md`.

## Git

- Commit or push only when the user asks. End commit messages with the Co-Authored-By line.

## Build

- `tools/build-plugin.sh <bbhost checkout>`: clang `--target=x86_64-w64-mingw32`, against the checkout's `include/`. Plugin API: `include/bbhost_plugin.h`. Param layouts: `include/bbhost/params.hpp`.

## Addresses

- Binary Ninja addresses (image base 0x400000) are the convention. bbhost `Rva` values: BN = rva + 0x400000.
- Eboot file offset = BN − 0x400000 + 0x4000.
- Runtime under bbhost on Windows: runtime = BN + 0x2ffc00000.

## Debugging

- **Don't pause the game in x64dbg**: more than ~2 s paused causes a GPU device-lost and bbhost exits. Use logging breakpoints instead: `bpcond addr, 0` plus `bplog addr, "...{p:expr}"`.
- Plugin hooks run on a host stack, not the guest stack; read guest arguments from `BbHookCtx`.

## Documentation standard

- Docs never list engine addresses, offsets or call sites, and never restate reverse engineering bbhost already documents (point to `include/bbhost/` instead). The addresses the plugin needs live only in `plugin/restock.c`'s `#define`s. Describe functions by role.
- `docs/findings.md` starts with the current state: what the plugin does, why it never creates items, and the item list with sources.
- Record in-game tests with their numbers from `restock.log` (inventory and storage, before and after).
- Pseudocode is plain C only.
- No abusable features in committed code (e.g. a restock-now button). Test-only code stays local.
- No local setup specifics in checked-in files (paths, drives, machine details).
