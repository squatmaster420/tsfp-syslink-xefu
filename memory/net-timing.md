# memory: network timing (send rate, full state, resend) — findings from the PC port

Status: **findings backported; receive-latency fix done; game-side timing changes not
yet implemented here** (they need a live session to locate the recompiled code — see
"Next step").

These findings come from the maintainer's crossplay branch of TheDoffman's PC port
(`timesplitters-future-perfect-pc-crossplay`, private), where the game's network code
was read and the timing changed and tested against xemu. They are about the **game's own
code**, so they hold for the Xbox-app release too: it runs the same title (45410066).

## Why remote players "teleport" in System Link

The game's own timing (USA disc build, guest addresses):

| What | Where | Value |
|---|---|---|
| Ticks per second | `0x4E8CEC` | 60 |
| Send epoch | `0x76B0F0` = tick `0x7497A8` / ((60 + 7) >> 3), set in `sub_003559A0` (`sar ecx,3` at `0x3559C0`) | changes every 7 ticks: **117 ms, 8.6 Hz** |
| Gameplay send gate | `sub_003544B0` at `0x35488B`: sends when `0x76B0F0` != `0x76B0EC` (last sent epoch) | once per epoch |
| Every-10-epochs task | `sub_003544B0` at `0x354936` (`add ecx,10`) → `sub_0035A480` | every 70 ticks |
| Movement sampler | `sub_003588D0`; record `+0x100` asks for a full state; `+0x10C` = match tick (`0x7DE318`) of the last | full state only when the controller bytes change, or **1 s** after the last; dead reckoning in between |
| Reliable resend | `sub_00352FF0`; queue at `[0x76ADF0]`, 2048 × 0x114; age `+0x108`, retries `+0x10C` | resend after **3 s** (3 × 60 ticks); give up after 20 (about 63 s) |
| Acknowledgements | `sub_00353AC0`, message 0x3D | every > 20 ticks (333 ms) or 32 pending |
| Duplicate filter | `sub_003532B0`: last 320 sequence numbers per peer | duplicates dropped, so extra resends are safe |

Likely causes of teleporting: the 117 ms send interval; full state only on input change,
so prediction drifts (collisions, knock-back) until the next change; a lost reliable
message waits 3 s.

Measured (PC client against xemu, 2026-11-16): in matches about 9.4 packets/s in,
19/s out. Teleporting also happens between two xemu instances, so it is the game's
behaviour, not the bridge's.

## The test settings ("Test values")

| Profile | Send rate | Full state | Resend after |
|---|---|---|---|
| Original hardware | 8.6 Hz (every 7 ticks) | on input change, ≤ 1 s | 3000 ms |
| Upstream PC values | 60 Hz | every 50 ms | 3000 ms |
| **Test values** | **20 Hz (every 3 ticks)** | **every 50 ms (every send)** | **750 ms** (limit raised to 80 so the give-up time stays ~60 s) |

Result (owner, 2026-11-17, PC client against xemu, including xemu hosting): Test values
**removed the visible stutter** of the PC player on the other screen. Message formats are
unchanged, so patched and unpatched machines play together; each patched machine makes
*its own* player smoother on everyone else's screen.

The same three changes as byte edits of the original XBE (for consoles and xemu):

| Edit | Address (disc) | Original | Test values |
|---|---|---|---|
| send epoch every n ticks | `0x3559C0` | `c1 f9 03` (sar ecx,3) | `6a 03 59` (push 3 / pop ecx) |
| keep the 10-epoch task at ~70 ticks | `0x354936` | `83 c1 0a` | `83 c1 17` (round(70/3)) |
| full state threshold | `0x35899B` | `8b 0d ec 8c 4e 00` (mov ecx,[FPS]) | `b9 03 00 00 00 90` |
| resend wait | `0x353029` | `a1 ec 8c 4e 00 8b ca 8d 14 40` | `8b ca ba 2d 00 00 00 90 90 90` |
| resend limit | `0x35306C` | `14` | `50` |

## The 10 ms receive delay, and this tool

In the PC port, a received frame was only signalled to the game on the emulated card's
next 10 ms timer tick; the fix raises the card's interrupt as soon as a frame arrives.

This tool already raised the "received" interrupt as soon as it copied a frame in, but its
bridge thread only looked at the socket when the guest signalled it or every **2 ms**
(`WaitForMultipleObjects(..., 2)`). Since 2026-11-18 the socket signals an event when a
datagram arrives (`WSAEventSelect(FD_READ)`), so the bridge wakes immediately; the timeout
is now a 50 ms safety net. Same change in the Cheat Engine script. Untested in play.

## Applying the timing changes here — next step

The Xbox-app title is **recompiled**: the original XBE bytes are in memory but are not the
code that runs, so the XBE edits above do nothing here. Two ways in:

1. **Patch the recompiled code** (preferred: race-free, like the XBE patch). In the big
   `xefu_*.dll`, find the recompiled forms of `0x3559C0`, `0x35899B`, `0x353029` and
   `0x35306C`: the recompiled code addresses guest data by absolute guest address, so
   search for the guest addresses as 32-bit displacements (`0x76B0F0`, `0x4E8CEC`,
   `0x76ADF0` ...) next to the same arithmetic, then change the immediates in memory the
   same way the injector patches `Emu.exe`.
2. **Write guest data from the bridge thread** (what the PC client does from inside the
   game loop): at 20 Hz while in a match (`[0x76B1A4]` == 15, scene `[0x79094C]` not
   101–107, `[0x7DE458] & 0x20000000` clear), set `[0x76B0EC] = [0x76B0F0] - 1` so the
   game's own send gate fires. Needs the guest→host mapping of guest RAM. Full-state and
   resend changes are harder from another thread (they need the sampler's peer records
   and exact before/after timing around the resend scan).

Both need the game running with Cheat Engine attached (see `emulator-internals.md`):
first confirm the build (disc vs. 2005 title update — the title update moves every address
above; the Live-section findings say "identical to the retail disc"), then locate the code.
