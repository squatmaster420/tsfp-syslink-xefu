# memory: Xbox Live revival (Insignia / xb.live)

Status: **System Link done; full Live on the Xbox-app emulator is BLOCKED.** Full
write-up for the service team: `insignia/FEASIBILITY.md`. Working files in
`insignia/` (`insignia-router.py`, `eeprom.py`, `NOTES.md`).

## The one-line result

The PC release ships the **complete, unmodified original Xbox Live client**, and a
**single byte** re-enables it — but Microsoft's emulator holds the console
identity in its recompiled runtime with **no injection point**, so it can't join.

## What works / what's proven

- **Live menu re-enable (one byte):** the recompiled title gates the "Xbox Live"
  menu entry against resource `IDS_ORIGINAL_XBOX_LIVE_TEXT` and shows "no longer
  available." Patch `jne`→`jmp` (`75`→`EB`) at **module offset `0x2440A48`** in
  `xefu_b9ab51a4…dll` → the real Live flow runs and reaches account selection
  ("New account"). (Module base is ASLR'd; find `xefu_b9ab51a4` each session.)
- **Live client intact:** `XONLINE`, `XNET`, `XON_RD` sections are byte-identical
  to the retail disc.

## The wall (why identity can't be injected)

Three independent findings:
1. No persisted identity: the title's local data holds only Connected-Storage game
   saves; no `eeprom.bin`/TDATA/account blob. The operating MAC changes every boot
   → identity is synthesised per launch.
2. No EEPROM image in memory: every occurrence of the live MAC is a packet buffer;
   no 256-byte EEPROM structure to overwrite.
3. Kernel calls are recompiled to **direct HLE calls**; the guest kernel thunk
   table (guest VA `0x475780`) is unused → no dispatch to hook.

So injecting the online key would need disassembling MS's recompiler/HLE (no
symbols). Same category as a dead end.

## The account also depends on identity

Account is provisioned/recovered **live** from the service using the console
online key (confirmed on a xemu drive: server says "already provisioned", keyed to
the serial). No portable local account blob. So the account inherits the identity
wall.

## xemu path (works today)

On xemu/hardware the EEPROM (online key) is user-controlled, so registration +
account creation succeed. To set that up we installed the MS dashboard into a
xemu `xbox_hdd.qcow2` by writing the XBL Dashboard Installer as the boot
`C:\xboxdash.xbe` (FATX write; see the qcow2/FATX helpers used in that work).

## Tooling

- `insignia/insignia-router.py` — host-side virtual LAN that plugs into the System
  Link bridge's frame tunnel: ARP + DHCP + DNS(→service) + ICMP + userspace TCP
  NAT. Self-test passes. (Network path = "piece C"; it's done. Pieces A=identity
  and B=account are the blocked ones.)
- `insignia/eeprom.py` — reads serial/MAC/online key from a real `eeprom.bin`
  (validated). HDD-key decrypt needs correct per-version keys (not needed for Live).
