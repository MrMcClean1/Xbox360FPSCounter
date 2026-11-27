# Xbox 360 FPS Counter

A realtime FPS counter for Xbox 360 games, loaded into each game by Peer Pressure's DLL
sideloading. It hooks the kernel's `VdSwap` (called once per frame) and has the GPU draw the
panel straight into the front buffer. The hook and GPU drawing are adapted from
[XenonFPS](https://github.com/pookatv/XenonFPS) (MIT).

## What it shows

- FPS, top left inside the TV-safe area (v1).
- Optional: min and average FPS over the last 10 seconds (v2), a frametime graph (v3), and
  CPU/GPU/RAM/board temperatures. Turn them on in `fps.ini`.
- Hold LB + RB + D-pad Down for half a second to step through: off -> FPS -> + MIN/AVG ->
  + graph -> + temperatures -> off. With the shipped `fps.ini` it starts off (`enabled = 0`).

The console can only replace pixels, not blend them, so the 50% backing box is a dither
(every other pixel black).

## Requirements

- An Xbox 360 running Peer Pressure, on kernel 2.0.17559 (the hook checks the `VdSwap`
  instructions first and does nothing on any other kernel).
- The Xbox 360 SDK (XDK) installed at `C:\Program Files (x86)\Microsoft Xbox 360 SDK`, or set
  `XEDK`. Not included.
- XexTool 6.3 (`xextool.exe`) in the project folder, for the retail conversion. Not included.
- Python 3 for `tools\make_inis.py`; Visual Studio 2010 for `test.bat` (optional).

## Install

1. `build.bat overlay` builds `out\fps_hello.xex`.
2. Copy Aurora's `Hdd1:\PeerPressure\Dashboard\Data\Databases\content.db` to the PC, then run
   `python tools\make_inis.py <path to content.db>`. It fills `out\sideload_all\` with one ini
   per Xbox 360 game, `fps_hello.xex`, `fps.ini`, and `games.txt` (which ini is which game).
3. FTP everything in `out\sideload_all\` to `Hdd1:\PeerPressure\SideLoading\`.

To turn it off for one game, rename that game's ini to `.off`.

## Layout

| Path | What it is |
| --- | --- |
| `overlay/overlay.cpp` | The console DLL: VdSwap hook, GPU command lists, fps.ini, toggle, temps, cleanup on exit. |
| `core/fps_core.c/.h` | Portable logic shared by the DLL and the PC tools: FPS counting, toggle, fps.ini parsing, min/avg, temperature decoding, the 5x7 font and the panel renderer. |
| `tests/test_core.c` | Unit tests for the core. |
| `tools/preview.c` | Draws the panel onto test frames pixel for pixel as the console does. |
| `tools/make_inis.py` | Makes the sideload inis for every game in Aurora's database. |
| `tools/ppm2png.py` | Converts previews to PNG. |
| `fps.ini` | Settings, copied to the console with the DLL. |
| `build.bat` | Console build (XDK). `build.bat overlay` = the counter, `build.bat` = the old test DLL in `hello/`. |
| `test.bat` | PC checks: unit tests and previews (`out\pc\preview_*.png`), with Visual Studio 2010. |
| `make-ini.ps1` | Makes one game's ini by hand. |

## Status

- [x] Core logic, tested
- [x] DLL sideloaded and DllMain running (base 0x8B000000)
- [x] v1: FPS drawn in-game (Saints Row, Far Cry 3), toggle combo, clean exit to Aurora
- [x] fps.ini, v2 min/avg, v3 frametime graph, temperatures, ini generator
- [x] Combo cycles off -> FPS -> + MIN/AVG -> + graph -> + temperatures (tested in Saints Row)

## Sideloading notes (learned the hard way)

- Put the DLL and its ini in `Hdd1:\PeerPressure\SideLoading\`. Name the ini `<titleid>.ini` in lowercase hex.
- The ini MUST include `ModuleVersion = "*.*.*.*"`. Peer Pressure treats a missing
  ModuleVersion as 0.0.0.0, not "any", so without it nothing ever matches.
- The XEX must be retail converted (`xextool -m r -r a`). `build.bat` does this.
- Title DLL base matters: at the default 0x88000000 DllMain never ran and Skate 3 hung.
  0x8B000000 works. The SDK only allows title DLL bases below 0x8C000000.
- Exiting a game with the Guide button does not call DLL_PROCESS_DETACH; the kernel's
  title-terminate notification is what undoes the hook.
- Toasts (XNotifyQueueUI) from the DLL never appear. Logs go to `Hdd1:\fps_diag.txt`.
- The kernel's fence-and-NOP block in the GPU ring is not always at the start of the block
  `VdSwap` hands over (seen 10 dwords in, in Saints Row). The hook searches for it; drawing
  only when it is at offset 0 skipped about 1 frame in 25, which showed as flicker.

## License

MIT, see `LICENSE`. Parts are adapted from XenonFPS (MIT); see `THIRD_PARTY_NOTICES.md`.
This is an unofficial homebrew project, not affiliated with Microsoft.
