# CLAUDE.md — index

TimeSplitters: Future Perfect, **Xbox-app (PC) release** — the original Xbox game
running inside Microsoft's Xbox-360 back-compat emulator (`Emu.exe`). This repo is
the **networking product**: System Link and Xbox Live revival. Quality-of-life
mods (resolution, FOV, ultrawide, bots, KB/M) live in the separate
[TSFP-Xefu-QOL](https://github.com/squatmaster420/TSFP-Xefu-QOL) repo.

This file is an **index**, not a dumping ground. Keep it lean. Put the real
detail in the per-area memory files under `memory/` and link them here.

## Workflow (read this every time)

On **every topic change**, before starting work:
1. Re-read this `CLAUDE.md`.
2. Check the index below for a memory file covering the area you're about to
   touch, and read it first — it has the how-to and the context (addresses,
   gotchas, what's been tried).
3. When you learn something durable about an area, update that area's memory
   file (and add a new one + index it here if it's a new area).

## Memory index — for topic X, read file Y

| Area | Read | Covers |
|---|---|---|
| How the emulator works, how we patch it, Cheat Engine MCP tips | `memory/emulator-internals.md` | Shared context for ALL the game/emulator work. Read first. |
| System Link / LAN multiplayer (the shipped tool) | `memory/system-link.md` | `src/` injector + bridge, `cheat-engine/`, build, how it works |
| Xbox Live revival (Insignia / xb.live) | `memory/xbox-live-revival.md` | Live client intact, one-byte re-enable, the identity wall. Full write-up: `insignia/FEASIBILITY.md` |

Quality-of-life mods were moved to the separate `TSFP-Xefu-QOL` repo; this repo
stays networking-only.

## Build

`sh build.sh` (needs LLVM: clang, lld-link, llvm-dlltool). Output: `dist/tsfp-syslink.exe`.
See `memory/system-link.md` for the toolchain detail.
