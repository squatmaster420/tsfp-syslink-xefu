# How it works

## The emulator

The Xbox app release (`Content\` folder of the install) is layered:

| Layer | Files |
|---|---|
| Launcher | `EmuMenu\EmuMenu.exe` (.NET); reads `LaunchArguments.txt` and `TitleMenuData.json` |
| Xbox 360 emulator | `Emu.exe` plus precompiled modules (`xeo3_*.dll`) and 360 system software in `Flash\` |
| Original Xbox emulator for the 360 | `SystemPartition\Compatibility\xefu.xex`, game code precompiled in `xefu_*.dll` |
| The game | `Game\DefaultPackage` + `DefaultPackage.data\Data0000..0024` (a signed package holding the original disc image) |

The game package is signed: changing even one byte of text in it stops the game
from starting. `LaunchArguments.txt` is not checked, but none of `Emu.exe`'s
options turns System Link on (`noNet`, `joinParty`, `sessionId` and others are
for Xbox network sessions).

## Why System Link says "no network cable"

`Emu.exe` has a `VirtualNicDevice` (one static instance). Its link-state method
returns `0x24` (connected) only when two fields, `_DeviceReady` (`+0x28`) and
`_PhysicalLinkConnected` (`+0x2C`), are set, and `0x20` otherwise. Nothing in
this build ever sets them, and nothing ever collects the frames the guest
queues: the event handles a backend would use (`+0x18`, `+0xAC8`) are null.

The guest reads the link state once while it boots and caches it, so setting
the fields later is not enough. The tool therefore:

1. waits for `Emu.exe` and immediately changes the `mov ebx,0x20` default in the
   link-state method to `0x24`, and sets both fields to 1;
2. starts a bridge that plays the part of the missing backend.

## The virtual NIC

Offsets from the primary object (`VirtualNicDevice`):

| Offset | Meaning |
|---|---|
| `+0x08` | interrupt status (read-and-clear by the guest) |
| `+0x18` | event signalled by the guest after it copies a received frame |
| `+0x28`, `+0x2C` | device ready, link connected |
| `+0x34` | u16 length of the frame waiting for the guest (0 = none) |
| `+0x36` | received frame |
| `+0x700` | transmit ring: 40 × `{u64 buffer; u32 length; u32 pad; u64 status*}` |
| `+0xAC0` | transmit producer index |
| `+0xAC8` | event signalled by the guest after it queues a frame |

A queued frame's status is `0x80000000` (pending). The bridge writes 0 when it
has sent it. Two helpers on the object's secondary interface set an interrupt
status bit and raise the NIC interrupt: `0x40` (used for "frame received") and
`0x14` (used for "frames sent"). A third (`0x10000`) is not used: raising it by
hand froze the game.

The frames are plain Ethernet II. The card's MAC address uses Microsoft's
original Xbox prefix `00:50:F2`. System Link discovery is the original Xbox
format: UDP broadcast from `0.0.0.1` to `255.255.255.255`, port 3074.

## The bridge

`src/bridge.c` is compiled to a single position-independent block of code (no
imports, no data, no relocations; the build fails if any appear). Everything it
needs, Win32 function addresses, the emulator addresses and its strings,
arrives in a `BridgeCtx` structure written next to it.

Its thread:

- waits on the two events it installed in the NIC object (2 ms timeout);
- sends every newly queued frame as one UDP datagram, marks it sent, and raises
  the "sent" interrupt;
- when the receive buffer is free, takes one datagram, copies it into the
  buffer and raises the "received" interrupt.

One Ethernet frame per UDP datagram is the format of xemu's *UDP Tunnel* and
what XLink Kai accepts on port 34523.

## Starting code inside the emulator

Creating a thread in the emulator from outside did not work, so the injector
borrows one of the emulator's own threads. It points the start of the function
that raises guest interrupts (called many times a second) at a small stub.
The first thread to get there calls `bridge_init`, which starts the bridge
thread. Every call then runs the function's original first instructions and
jumps back. Once the bridge reports that it is running, the injector puts the
original bytes back.

Code is only written while all of the emulator's threads are suspended and none
of them is part-way through the bytes being changed.

## Finding things

Everything is located by byte pattern, then cross-checked against the live
object: the NIC's vtable must point at the found link-state method, its second
vtable at the two interrupt helpers, and both helpers must jump to the found
interrupt function. If anything does not match, the tool stops without writing.
