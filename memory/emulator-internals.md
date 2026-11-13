# memory: emulator internals & how we patch

Shared context for all game/emulator work. Read before `system-link`,
`xbox-live-revival`, or `qol-mods`.

## What the target is

- The PC "Xbox app" release runs the original Xbox game inside Microsoft's
  Xbox-360 backward-compat emulator, process **`Emu.exe`**.
- The game title is **recompiled** ahead of time into `xefu_*.dll` modules (the
  big one is `xefu_b9ab51a4…dll`, ~74 MB — the converted TimeSplitters). The
  original Xbox code's data/sections are also present and run; we can read and
  patch them.
- Install path (this machine): `E:\Fireteam Elite\TimeSplitters Future Perfect\Content\`.
  Package build `2610.3007.1901.0`, title id `45410066`.
- The on-disc game package is under `Content\Game\DefaultPackage*`; extract XBEs
  with `research/tsfp_extract.py` (gap auto-detect works).

## How we interact with it

- **Cheat Engine + its MCP bridge** (`mcp__remote-devices__cheatengine__*`) is the
  main tool: `evaluate_lua`, `aob_scan`, `aob_scan_module`, `read_memory`,
  `find_references`, `disassemble`, etc. Shell exec is disabled in the bridge.
- **Device Linux shell** (`mcp__remote-devices__device_bash`) is a separate Linux
  VM with the Content folder mounted — good for reading/parsing files, NOT for
  Windows processes.
- Always `openProcess(getProcessIDFromProcessName("Emu.exe"))` at the start of a
  CE Lua session.

## CE bridge gotchas (important)

- **Heavy operations time out the bridge (~80s) and can wedge it.** What times
  out: `evaluate_lua` doing many full-memory `AOBScan`s in one call; whole-process
  `writeRegionToFile`; big unknown-value mem-scans. Symptom: "Cheat Engine Bridge
  … is not reachable" or "device did not respond". Recovery: wait ~45s, `ping`;
  if still wedged, ask the user to clear any CE error dialog / restart the bridge.
- **Prefer the `aob_scan` MCP tool** (fast, pattern) over `evaluate_lua` AOBScan.
  Use `aob_scan_module` to scope to a module (e.g. `Emu.exe`).
- Writing a large file: split into ≤8 MB chunks (`writeRegionToFile` per chunk).
- A CE error dialog (e.g. failed file create) blocks the Lua engine until dismissed.

## Addressing & ASLR

- Host addresses change every launch (ASLR). Re-derive anchors each session;
  never reuse absolute addresses from a previous run.
- Guest→host mapping is not a simple contiguous offset you can assume; find
  structures by content/pattern each time.
- `Emu.exe` base this-session example: `0x7FF7D0A60000` size `0xDA3000`. The
  `VirtualNicDevice` global sits at `Emu.exe + 0x12C2B8` (drives System Link).

## Patching model

- Memory patches are **reversible** — they live only in the running process and
  vanish when the game closes. Save originals before writing.
- Kernel calls in the recompiled title are translated to **direct HLE calls**;
  the guest XBE kernel thunk table (guest VA `0x475780`) is **unused**, so there's
  no thunk to hook (this is why identity injection is blocked — see
  `xbox-live-revival.md`).

## Reversible-test etiquette with the user

- The user drives the game. For "does this address do X?" tests, pulse a value
  between original and test with `sleep()` so the effect is visible, then restore.
- Interactive value-scans need the user to change game state on cue (e.g. ADS
  zoom); keep rounds few and tell them which direction each scan expects.
