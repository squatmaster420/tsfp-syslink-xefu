# Insignia on the Xbox-app emulator — research notes

**Goal:** let the PC Xbox-app release of TimeSplitters: Future Perfect (the game
running inside Microsoft's Xbox-360 back-compat emulator, `Emu.exe`) connect to
Insignia, by presenting a console that was registered on xemu.

**Status: research / partial. Not a working connection yet.** Two of the three
pieces still need live reverse-engineering of the emulator, and the account
format is Insignia's own knowledge. This folder is experimental and separate
from the shipped System Link tool.

## What was established (on this machine, game running)

1. **The "Xbox Live is no longer available" message is a single check Microsoft
   added to the converted game**, not a removal of code. The converted title
   module (`xefu_b9ab51a4…dll`) tests whether the selected menu item is the
   "Xbox Live" entry and, if so, shows `IDS_ORIGINAL_XBOX_LIVE_TEXT` instead of
   running the feature. Bypassing it is a one-byte change in memory
   (`75 → EB`, i.e. `jne → jmp`, at module offset `0x2440A48`).

2. **The original Xbox Live client is present and intact.** The game's
   `XONLINE`, `XNET` and `XON_RD` sections in memory are byte-for-byte identical
   to the retail disc. With the check bypassed, the Live menu runs: the game
   enumerates accounts, finds none, and offers **New account** — exactly as on
   real hardware with no account saved.

3. **The console identity is ephemeral.** No `eeprom.bin`, no TDATA, no account
   blob is persisted (the title's local data holds only Connected-Storage game
   saves, mapped to `U:\45410066`). The NIC MAC differs every boot
   (`…28:0E:BC` then `…27:98:0C`), i.e. the emulator synthesises a throwaway
   identity in RAM each launch. There is no static EEPROM buffer carrying the
   MAC in host memory.

4. **"New account" quits to the dashboard** (which this emulator does not have),
   so the game closes. Account *creation* is therefore impossible in-emulator.

## Plan (do signup on xemu; inject at launch)

```
   xemu (once)                         Xbox-app emulator (every launch)
   ───────────                         ────────────────────────────────
   register console with Insignia  →   inject identity into RAM  (piece A)
   create Live account             →   present saved account     (piece B)
                                       virtual LAN + DNS→Insignia (piece C)
```

The user supplies the files xemu produced (`eeprom.bin`, and the account from
xemu's virtual HDD); the launcher reads them and patches the running emulator.

### Piece A — identity injection  *(needs live RE)*
Override the serial + online key (+ HDD key if used) the emulator hands the
guest, with the values from the user's registered `eeprom.bin`. Because the
identity is HLE-synthesised, not a buffer, the target is the emulator's
non-volatile-setting provider / the in-RAM identity it builds at boot. **Finding
that target is the main open task.** `eeprom.py` already reads the needed fields
from the user's file.

### Piece B — saved account  *(needs live RE + Insignia knowledge)*
Make the game's account enumeration return the account created on xemu, so it
logs in instead of forcing New-account. Needs (a) where the emulator lets the
guest read accounts, and (b) the account blob format — the latter is Insignia's
domain.

### Piece C — virtual LAN / routing  *(built, testable now)*
`insignia-router.py` plugs into the bridge's existing frame tunnel (set the ini
`[relay] port` to the router). It answers ARP, runs DHCP (gives the console a
real IP + itself as DNS), resolves the `*.XBOXLIVE.COM` names to Insignia, and
replies to ICMP. TCP NAT to the real Insignia servers is the next milestone;
for now it logs the console's connection attempts so we can see its behaviour.

## What I need to continue
- A xemu install where Insignia registration + account creation were completed,
  and its `eeprom.bin` (+ the account data / virtual HDD) — the inputs for
  pieces A and B, and so the injected console is a *real* registered one.
- From the Insignia side: how a provisioned account is stored/validated, so
  piece B injects something the game and the servers both accept.

## Files
- `insignia-router.py` — virtual LAN/DNS router (piece C). Self-test passes.
- `eeprom.py` — reads serial / MAC / online key (and HDD key) from `eeprom.bin`.
