# TimeSplitters: Future Perfect (PC Xbox-app release) — Xbox Live revival feasibility

*Technical findings, prepared for the Insignia / xb.live team.*
*Investigation on Windows with Cheat Engine + memory analysis. All emulator work read-only unless noted.*

## Summary

The PC release ships the **complete, unmodified original Xbox Live client**, and a
single byte re-enables it. The only thing preventing it from joining the service
is that Microsoft's back-compat emulator holds the console identity inside its
recompiled runtime with **no injection point exposed**.

| Area | Status |
|---|---|
| System Link (LAN) over XLink Kai | **Working** — plays with PC, xemu and real Xboxes |
| Live client present | **Confirmed** — original `XONLINE`/`XNET` code intact and runs |
| Live menu re-enable | **Confirmed** — one-byte patch; reaches account selection |
| Console identity injection | **Blocked** — no hook in MS's recompiled kernel/HLE |
| Account on Xbox-app emulator | **Blocked** — recovered live via online key → inherits the identity blocker |
| Account on xemu / hardware | **Working** — EEPROM is user-controlled there |

## 1. Context

The Xbox-app ("PC") version is the original Xbox game running inside Microsoft's
Xbox 360 backward-compatibility emulator (`Emu.exe`), with the title pre-recompiled
into `xefu_*.dll` modules. Package tested: build `2610.3007.1901.0`, title id
`45410066`. This is the same emulation family used for original-Xbox back-compat on
Xbox 360 / One / Series — which Insignia does not currently support.

## 2. What works — System Link

The emulator's virtual NIC is present but reports its cable "disconnected," so System
Link shows *"no network cable attached."* Patching the link-state default
(`0x20`→`0x24`) and bridging the card's Ethernet frames over UDP (one frame per
datagram — the XLink Kai / xemu "UDP Tunnel" format, port `34523`) restores full
System Link play. Memory-only and reversible.

## 3. Key finding — the Live client is intact

With the Live menu re-enabled, the game runs its **original** Xbox Live code path:
it initialises the network stack, enumerates accounts, finds none, and offers
*"New account"* — exactly as retail hardware with no account saved.

- The game's `XONLINE` (Live client), `XNET` (network stack) and `XON_RD` (Live
  read-only data, incl. the `MACS/AS/TGS.XBOXLIVE.COM` names) sections in memory are
  **byte-for-byte identical to the retail disc**. Nothing was stripped.
- The "Xbox Live is no longer available" message is a single gate Microsoft added to
  the recompiled title: it compares the selected menu entry against the resource
  `IDS_ORIGINAL_XBOX_LIVE_TEXT` and, if matched, shows the message instead of running
  the feature.
- Re-enabling it is a **one-byte change** (`jne`→`jmp`) in the recompiled title
  module. After it, the real Live flow executes.

> The PC release is not missing Live support — it is deliberately gated, behind an
> otherwise functional client.

## 4. The blocker — an un-hookable console identity

On real hardware or xemu, the console's identity (serial + online key, from the
EEPROM) binds a registration and lets the service recover the account. On the
Xbox-app emulator that identity is **synthesised per boot and exposed nowhere we can
reach**. Three independent approaches confirm this:

- **Persisted identity on disk** — none. The title's local data holds only
  Connected-Storage game saves. No `eeprom.bin`, no TDATA, no account blob. The
  operating NIC MAC differs every launch → identity is regenerated each boot.
- **EEPROM image in memory** — none. Every occurrence of the live MAC is a network
  packet buffer; no 256-byte EEPROM structure exists to overwrite.
- **Kernel thunk table (the intended hook)** — unused. The game's XBE kernel-thunk
  pointer decodes to guest VA `0x475780`, but the table is empty: the title is
  *recompiled* and kernel calls (incl. `ExQueryNonVolatileSetting`, which reads the
  online key) are translated to **direct calls into the emulator's HLE runtime**.
  There is no dispatch indirection to intercept.

So the online key is produced deep inside Microsoft's undocumented recompiler/HLE,
with no label, no standing buffer, and no thunk to hook. Overwriting it would require
disassembling that runtime (tens of MB, no symbols) to locate the identity handler
and its backing store.

## 5. The account inherits the same dependency

A console registered and provisioned on xemu showed the account is **not** stored as
a portable local file — the provisioning tool recovers it *live* from the service
using the console's online key (server returns "account already provisioned," keyed
to the console serial). The account can only come back if the client presents the
correct online key — the very value that cannot be injected into the Xbox-app
emulator. The account problem collapses into the identity problem.

## 6. xemu / hardware vs. the Xbox-app emulator

On xemu and real consoles the EEPROM is user-supplied, so the online key is whatever
you register — and provisioning works today. The Xbox-app emulator manufactures its
own throwaway identity and offers no equivalent input. This is the precise reason the
PC release can't currently join, and it is independent of the Live client.

## 7. What a solution would require

- **Identity:** reverse-engineer Microsoft's recompiled-kernel HLE path for
  `ExQueryNonVolatileSetting` (and the EEPROM factory fields) to find where the online
  key / serial are sourced, then inject a registered console's values at launch. No
  symbols; substantial effort; feasibility not guaranteed.
- **Network:** already solved in prototype — `insignia-router.py` answers DHCP,
  resolves the Xbox Live hostnames to the service, and NATs the console's TCP
  connections through. Self-tested.
- **Account:** once identity is injected, the game's own Live client performs
  provisioning/recovery against the service — no separate account porting needed.

## 8. Tooling produced

- **System Link tool** — launcher + in-emulator bridge (`src/`). Working, reversible.
- **Virtual router (prototype)** — `insignia/insignia-router.py`: DHCP + ARP + DNS
  (→service) + ICMP + userspace TCP NAT, plugging into the bridge's frame tunnel.
- **EEPROM reader** — `insignia/eeprom.py`: extracts serial / MAC / online key from a
  registered `eeprom.bin` (validated against a real file), ready as the
  identity-injection input if the HLE hook is ever found.

## 9. Recommendation

For players today, online TimeSplitters with a provisioned account works on **xemu or
hardware**. The PC Xbox-app release is a strong candidate for future support — the
client is complete and one byte from live — but joining it requires either
Microsoft-emulator internals work on the identity layer, or coverage of the
back-compat family as part of the planned original-Xbox-on-360 effort (same emulator
lineage).

---

*Console-specific identifiers (serial, online key, account email/gamertag) are
intentionally omitted. Memory offsets are build-specific to package
`2610.3007.1901.0` and will differ across versions and launches.*
