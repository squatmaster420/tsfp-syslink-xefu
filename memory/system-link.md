# memory: System Link / LAN multiplayer (shipped tool)

Status: **working and shipped.** Plays over XLink Kai with PC, xemu and real
Xboxes in the TimeSplitters arena.

## What it does

The emulated virtual NIC is present but reports its cable "disconnected," so the
game's System Link shows *"no network cable attached."* The tool:
1. Patches the NIC link-state default so the cable reads "connected".
2. Bridges the card's raw Ethernet frames over UDP — one frame per datagram, the
   XLink Kai / xemu "UDP Tunnel" format — to XLink Kai (default `127.0.0.1:34523`,
   listens on `9369`).

All memory-only and reversible.

## Files

- `src/injector.c` — launcher: reads `tsfp-syslink.ini`, launches the game, waits
  for `Emu.exe`, patches the NIC, injects the bridge payload, reports status.
  Reads ini values with inline `;`-comment stripping (`clean_value`).
- `src/bridge.c` — position-independent payload injected into `Emu.exe`: finds the
  NIC, holds the link flags, tunnels TX frames out / injects RX frames. Must be
  ONE relocation-free `.text` section (no data section, no imports).
- `src/shared.h` — shared structs + `STAGE_*` codes the injector prints.
- `tools/embed.py` — extracts the bridge `.text` straight from the COFF object and
  emits `payload.h` with entry offsets. **Do NOT use `llvm-objcopy -O binary` on a
  COFF object** — it emits the whole file incl. headers, so the code lands at the
  wrong offset and the hook jumps into mid-instruction (this was build 1's crash).
- `cheat-engine/tsfp_syslink.lua` — pure-CE alternative that does the same thing;
  start the bridge only once the guest has queued its first frame (early start
  crashes the boot) and keep holding the NIC flags.
- `docs/how-it-works.md` — longer narrative.

## Build

`sh build.sh` → `dist/tsfp-syslink.exe` (v1.1.0). Needs LLVM clang / lld-link /
llvm-dlltool. GitHub Actions `.github/workflows/build.yml` builds on release.

## Receive latency

The bridge wakes on a Winsock socket event (`WSAEventSelect(FD_READ)`), not a 2 ms poll;
see `memory/net-timing.md`.

## Receive buffer and lost frames (2026-11-21)

Owner, Xbox app with a real Xbox, Capture the Bag: on his screen a bag carrier died and
the bag reset, but the carrier went on holding it (the machines disagreed). Not the PC
port's cause (upstream's payload guard; that repo only). Not identified here. Weak point
found: the card takes one received frame at a time and the socket had Windows' default
receive buffer, which drops a burst silently. Now 1 MB (`SO_RCVBUF`), and the status line
shows the most bytes ever waiting (`FIONREAD` after each hand-over). Next: play a Capture
the Bag match and read `waiting at most N of 1024 KB`; if N stays small, frames are not
lost here and the cause is elsewhere (loss on the internet path through XLink Kai, or the
game itself). The Cheat Engine script does not have this yet.

## Key facts

- `VirtualNicDevice` is a static global at `Emu.exe + 0x12C2B8`.
- Link-state patch: NIC flags `0x20` → `0x24` (cable connected).
- The bridge must start only after the guest's network driver has queued its
  first frame, or the boot crashes.
- Build 2 (crash on boot) root cause was the embed/objcopy bug above — fixed.
