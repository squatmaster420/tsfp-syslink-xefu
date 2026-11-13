# tsfp-syslink

System Link for **TimeSplitters: Future Perfect** in the Xbox app on PC.

The PC release runs the original Xbox game inside Microsoft's Xbox emulator. The
emulator includes a virtual network card, but leaves it switched off, so System
Link stops at *"You cannot play a System Link game, because there is no network
cable attached to this Xbox Console."* This tool switches the card on and
connects it to **XLink Kai** (or to **xemu**), so you can host and join System
Link games.

- No files are changed. Everything happens in the emulator's memory while the
  game runs; closing the game undoes it.
- One small `.exe`, no install, no Cheat Engine.
- Plays with other PC players, xemu and real Xboxes through XLink Kai
  (they need the same game version, the US release).

> Unofficial and not affiliated with Microsoft, Free Radical Design, Deep Silver
> or Team XLink. Use at your own risk, and only for System Link/LAN play.

## Download

Get `tsfp-syslink.exe` from the [Releases](../../releases) page.

Windows may warn about it: it is unsigned and writes into another program's
memory, which is exactly what it has to do. The full source is in this
repository, and every release is built from it by GitHub Actions.

## Use

1. Install and start [XLink Kai](https://www.teamxlink.co.uk/) (run it as
   administrator and allow it through the firewall).
2. Close TimeSplitters if it is running, then run `tsfp-syslink.exe`.
   It starts the game for you and must stay open while you play.
3. Wait for `Bridge running` in its window.
4. In XLink Kai, go to the **TimeSplitters: Future Perfect** arena.
5. In the game: **Multiplayer -> System Link**, then host or search.

The window shows how many frames have been sent and received. Kai's
**Metrics** tab should show traffic as soon as the game is on the System Link
screen.

## Settings

The first run creates `tsfp-syslink.ini` next to the exe:

```ini
[relay]
host=127.0.0.1      ; machine running XLink Kai (or xemu)
port=34523          ; 34523 = XLink Kai, 9368 = xemu
local_port=9369     ; UDP port this tool listens on

[game]
launch=1            ; 1 = start the game automatically
app=Xbox360BackwardCompatibil.PrimaryTimeSplittersFutu_ksqcvrsvwz2jp!App
```

| Setup | host | port |
|---|---|---|
| XLink Kai on this PC (default) | `127.0.0.1` | `34523` |
| XLink Kai on another PC | that PC's LAN IP | `34523` (enable *Allow Remote DDS Connections* in Kai) |
| xemu on this PC, no Kai | `127.0.0.1` | `9368` |

For the xemu setup, set xemu's network to **UDP Tunnel**, bind address
`127.0.0.1:9368`, remote address `127.0.0.1:9369`, and use the US disc image.

## Troubleshooting

- **"This version of the emulator is not supported"**: the Xbox app updated
  the emulator and the tool no longer recognises it. Nothing was changed.
  Open an issue with the game version shown in the Xbox app.
- **"The game is already running"**: the fix must be applied while the game
  starts. Close it and run the tool again.
- **"Cannot open Emu.exe"**: run the tool as administrator.
- **"Could not use UDP port 9369"**: another program uses that port; change
  `local_port`.
- **No games found through Kai**: check Kai's Metrics tab. If the game's
  traffic shows up there, the tool is working and the issue is on the Kai or
  network side.

## Build

Needs LLVM (`clang`, `lld-link`, `llvm-dlltool`) and
Python 3. Works on Linux, WSL, or Windows with LLVM installed (use Git Bash):

```sh
sh build.sh        # -> dist/tsfp-syslink.exe
```

No Windows SDK or C runtime is used.

## Repository layout

| Path | |
|---|---|
| `src/injector.c` | the exe: finds the emulator, patches it, loads the bridge |
| `src/bridge.c` | the bridge that runs inside the emulator |
| `src/shared.h` | data shared between the two |
| `cheat-engine/tsfp_syslink.lua` | the same thing as a Cheat Engine script |
| `research/tsfp_extract.py` | reads the game package (`DefaultPackage.data`) |
| `docs/how-it-works.md` | technical details |

## Supported versions

Tested with the Xbox app package `2610.3007.1901.0`. The tool finds what it
needs by searching for byte patterns rather than fixed addresses, so small
updates may keep working; if the patterns are not found it stops without
changing anything.

## License

MIT, see [LICENSE](LICENSE). This repository contains no Microsoft or game files.
