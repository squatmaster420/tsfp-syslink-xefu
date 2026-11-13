# memory: quality-of-life mods

Exploration of QoL features for the emulated game. Two layers: **game-code
patches** (our wheelhouse — intact Xbox code, findable constants/flags) and
**emulator-pipeline** changes (MS's recompiled renderer — deep, no anchor).

User-set priority order: **resolution → FOV → ultrawide → bots → KB/M.**

## Feasibility summary

| Feature | Layer | Verdict |
|---|---|---|
| Higher resolution (>2560×1920) | emulator renderer | Hardest; no game-code anchor |
| FOV control | game code | Tractable; projection is non-standard (see below) |
| Ultrawide 21:9/32:9 | game + emulator | Doable for 3D view; gated by emulator 16:9 output lock + HUD |
| Bots in System Link | game code (menu only) | **Just a menu unlock** — user confirms netcode already syncs bots |
| Native KB/M | game code | Keyboard easy; real mouse-look = moderate RE |

## Resolution — findings

- Model confirmed: game renders **native 640×480**; emulator **supersamples 4×**
  to **2560×1920**. Both render-target descriptor types exist in heap
  (native `80 02 00 00 E0 01 00 00`; 4× `00 0A 00 00 80 07 00 00`).
- A render-target descriptor stores dims as float (`+10`=2560.0,`+14`=1920.0) and
  int (`+28`), plus format/flags.
- **No static `2560×1920` constant in `Emu.exe`** (checked int + float) → the cap
  is computed (native × 4) and clamped in the renderer. Raising it = find/patch
  the scale/clamp in MS's renderer (likely needs a write-breakpoint on the buffer
  sizing), applies at init (relaunch), some risk. Deep-RE category.
- Config files expose no resolution knob (`LaunchArguments.txt` only has
  `windowAspectRatio=16:9-10x11`).

## FOV — findings & NEXT STEPS

- The engine does **not** use a standard D3DX perspective matrix: signature scans
  for LH (`1.0` at m23), RH (`-1.0`), and transposed layouts all failed to find
  the world projection. It rolls its own.
- Found a camera **template** block (this-session addr `~0x1EF6C59CD8C`) with
  frustum params: `+18`=4.0 (width), `+28`=3.0 (height), `+20`=1.3333 (aspect
  4:3), far `200`, and native `640×480` as ints at the end. **BUT editing it had
  no visual effect** → it's a static/dead template, not the live projection.
- The game is 16:9 but only the **4:3** aspect constant (`AB AA AA 3F`) appears in
  guest memory, not 16:9 (`39 8E E3 3F`) → engine widens from a 4:3 base.
- Interactive value-scan via ADS zoom **did not converge** (33M→20M→12M): the
  emulator's host memory is too volatile; scanning the whole writable range is
  hopeless.
- **NEXT:** constrain the unknown-value scan to the **guest Xbox RAM** (the 64 MB
  Xbox memory where game variables live), not the whole process. Locate that
  region first (CE `enumMemoryRegions` field names need checking — the naive
  `>=32MB` filter returned empty; try `enum_memory_regions_full` or inspect the
  region table fields). Then unknown-value → zoom-out `increased` → zoom-in
  `decreased`, 2–3 rounds, should isolate the FOV float. Then the projection/
  aspect patch for FOV control + ultrawide lives near it.
- Watch-outs: weapon viewmodel + HUD are tuned for default FOV; extreme values
  clip/stretch them (tunable, not blockers).

## Ultrawide — plan

- Same projection code as FOV; find FOV first (it hands us the aspect term).
- Blocker to check early: `MicrosoftGame.config`/`LaunchArguments` lock output to
  `16:9` — presenting a true 21:9/32:9 frame may need an emulator-side change.
- HUD/menus laid out for 4:3/16:9 will sit at the edges (usual ultrawide tradeoff).

## Bots in System Link — plan

- User confirms the netcode **already syncs bots online** — so this is purely a
  **UI unlock**: expose the "add bots to teams" option in the System Link match
  setup (it's gated off vs. local Arcade). Find the menu gate that hides the bot
  option in System Link mode and flip it. No netcode work needed.

## KB/M — plan

- Keyboard→buttons + mouse→stick mapping is easy (inject into the emulated pad;
  the injector can already read host input).
- Real raw mouse-look = patch the game's camera/aim update to consume mouse deltas
  directly (bypass stick). Moderate RE, high value; depends on finding the camera/
  aim code (overlaps with the FOV hunt).
