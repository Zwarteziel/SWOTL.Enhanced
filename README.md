# SWOTL Enhanced

Version 0.1.14

Run **Secret Weapons of the Luftwaffe** (Lucasfilm Games, 1991) on modern Windows
without installing DOSBox. You need your own copy of the original game files.
Tested with the original 1991 release and with the later 1994 release included in
Lucasarts Air Combat Classics that included the P-38, P-80, He 162 and Do 335.

The original `SWOTL.EXE` and its overlay files run inside a small PC emulation
layer made for this one game. The layer covers the x86 CPU, DOS, BIOS, VGA, AdLib,
joystick and mouse. SDL3 handles the window, scaling, sound and gamepads.

## What works

- Intro, menus, campaign map, briefing and flight. The game was tested up to the
  3D flight with cockpit and controls.
- **Display.** Window or full screen, with the original 4:3 shape. Three scaling
  styles: *sharp* (crisp pixels with smooth edges), *nearest* and *linear*.
- **CRT shader.** By default the picture looks like a 1990s VGA monitor, with
  the CRT shaders and colour emulation of DOSBox Staging 0.83 (`shader =
  crt-auto`). See *CRT shader* below.
- **Gamepad as joystick.** The game's start-up joystick calibration is answered
  automatically.
- **AdLib music and sound effects.** Emulated with Nuked-OPL3.
- **Mouse.** The mouse is captured when you click in the window.
- **Speed.** The title bar shows the game's speed (`speed`): emulated time
  against real time. At 100% the game runs exactly as fast as on a real PC. If your PC cannot
  keep up, it drops below 100% and everything gets slower, the flying too.
- **Frame rate.** The emulated CPU speed adapts to your PC. The title bar shows
  the real 3D frame rate (`fps`; a dash outside the 3D view). It counts the game's own frame-present calls in the VGA
  driver. At 10 MIPS it is about 16 fps, at 20 MIPS about 34 fps and at 40 MIPS
  about 63 fps. The flight model runs on the game's 237 Hz timer, so the frame
  rate does not change how the aircraft handle.
- **Keyboard flying.** By default the arrow keys drive a smooth virtual
  joystick while you fly. Held down, the stick reaches full deflection in 0.25 s;
  released, it springs back to the centre in 0.12 s. Hold an arrow and the
  aircraft keeps rolling, as in the original. With `keyboard_flight=classic` the
  arrows go to the game as key presses, and every key repeat is one control
  step. Held keys repeat 30 times per second after 250 ms. That is the fast
  typematic setting of most 386/486 BIOSes and what DOSBox gives on Windows.
  With the slow power-on default of the keyboard (10.9 per second after 500 ms),
  the aircraft rolls about three times more slowly.

- **Modified game copies.** Some copies of SWOTL have a modified
  `NOTCAMP.OVL` that only works when the program is loaded at certain memory
  addresses. SWOTL Enhanced loads the game at an address where it works, so you
  get the *Choose a side* screen and clean menu text, just as in DOSBox.
  Unmodified copies are not affected.

The version number is shown in the title bar, at the top of
`swotl-enhanced.ini` and in the file properties of the exe (right-click >
Properties > Details).

## Installation

1. Copy `SWOTL-Enhanced.exe` into the folder that holds `SWOTL.EXE` and the
   `AC`, `CP`, `FE`, `FX` and `MIS` folders.
2. Start `SWOTL-Enhanced.exe`.

On first start the program writes `swotl-enhanced.ini` next to the exe. All
settings are in that file, each with a short explanation. When you start a new
version for the first time, it rewrites the file in its own layout: your values
stay as they are, new settings are added, and every setting gets its current
explanation. The old file is kept as `swotl-enhanced.ini.bak`. Your own
comments in the file are not carried over.

## Keys added by SWOTL Enhanced

| Key | Action |
|---|---|
| Alt+Enter or F11 | Full screen on/off |
| Ctrl+F10 | Release or capture the mouse |
| F12 | Screenshot (`swotlNNNN.bmp`) |
| Ctrl+F11 / Ctrl+F12 | Lower / raise the CPU speed limit |
| Ctrl+F9 | Show speed, frame rate and CPU speed in the picture (they are always in the title bar) |
| Ctrl+F6 | CRT shader on/off (saved in the ini file) |
| Ctrl+F7 / Ctrl+F8 | Make the gamepad stick less / more curved (saved in the ini file) |
| Ctrl+F5 | Start / stop a diagnostic recording (`swotl-rec-NNNN.avi` + `.log`, max 60 s) |

All other keys go to the game. See `SWOTL.DOC` for the game's own keys.

## Default gamepad layout

| Gamepad | Game |
|---|---|
| Left stick | Joystick (in menus: mouse pointer) |
| A | Fire button 1 (in menus: click) |
| B | Fire button 2 |
| X | Enter (drop bombs / rockets) |
| Y (hold) | Look back |
| Right stick | Look around (keypad views) |
| RT / LT | Throttle up / down |
| RB / LB | Rudder right / left (needs `advanced=1`) |
| D-pad | Flaps (up), gear (down), map (left), gun camera (right) |
| Start | Pause (Alt+P) |
| Back | Esc |

You can change every button in the `[gamepad]` section of the ini file.

**Rudder.** SWOTL has a rudder only with advanced flight controls: set
`advanced=1` in `[game]`. Without it the rudder keys (F5-F9) do nothing, and a
message in the window says so. LB and RB give full rudder while held and centre
it when released. For an **analog rudder on the triggers**, set `lt=RUDDER` and
`rt=RUDDER`, and move the throttle to the bumpers with `lb=MINUS` and `rb=PLUS`.
The game itself only knows partial and full rudder, so SWOTL Enhanced turns the
trigger into rudder key presses: a slight pull gives partial rudder, a further
pull switches between centre and full rudder ten times a second in the ratio of
the pull, and a full pull gives full rudder. In a P-51B over 6 seconds, a 15%
pull turns about 4 degrees, 50% about 35 and a full pull about 76.
Triggers can also take `KEY1/KEY2` (held like a button, e.g. `lt=F5/F9`).

**Stick response.** By default the left stick is linear, just like a joystick
from 1991. `stick_curve` (1.0 to 3.0) bends the response: small movements
around the centre give finer control, and full stick still gives full
deflection. Try 1.5 to 2.0. `stick_range` (0.3 to 1.0) limits the maximum
deflection. Ctrl+F7 / Ctrl+F8 change the curve in steps of 0.25 while you fly.

**Diagnostic recording.** Ctrl+F5 records the game picture (320x200, 30 fps,
uncompressed AVI) to `swotl-rec-NNNN.avi` in the game folder. The top-left
corner of the picture shows the input state. A `.log` file next to it lists, for
each frame, the timing (speed, fps, CPU speed), the keys and the stick values
sent to the game. A recording of 20 seconds is about 115 MB.

## CRT shader

SWOTL Enhanced uses the CRT shaders of DOSBox Staging 0.83.0 and chooses
between them in the same way as its `shader = crt-auto` setting does for VGA
games. A real VGA monitor drew every line of the 320x200 picture twice
(double scanning), so a convincing scanline effect needs at least two to three
screen pixels per line. The variant therefore depends on the height of the
picture:

| Height available for the picture | Variant |
|---|---|
| 1600 pixels and more (4K screens) | `crt-hyllian`, VGA 4K preset |
| 1200 to 1599 pixels (1440p screens) | `crt-hyllian`, VGA 1440p preset |
| 800 to 1199 pixels (1080p screens) | `vga-1080p-fake-double-scan` |
| less than 800 pixels | no CRT effect, plain `scale` |

As in DOSBox Staging, the picture is scaled in whole or half steps where that
gives cleaner scanlines, and the colours are adjusted to those of a VGA
monitor (P22 phosphors, 7800 K white point). When the window changes size, the
variant can change; a short message shows the new one. `shader=crt-1080p`,
`crt-1440p` or `crt-4k` forces one variant. `shader=none` switches the effect
off. Ctrl+F6 switches between `crt-auto` and `none`.

The shaders need OpenGL 3.3, which every PC graphics card from the last 15
years supports. If OpenGL does not work, the program falls back to its normal
renderer without the effect, and writes the reason to
`swotl-enhanced-gl.log`. `renderer=sdl` always uses the normal renderer.

## Settings (`swotl-enhanced.ini`)

- `mips`: the highest emulated CPU speed. The default is 40. About 20 is a fast
  486. More speed gives a smoother 3D view.
- `detail`: ground detail, 1 to 5.
- `advanced`: advanced flight controls (rudder).
- `joystick`: `auto`, `on` or `off`. With `auto`, the game's joystick is on
  when a gamepad is connected or `keyboard_flight=stick`. With `off`, the arrow
  keys always work as classic keys.
- `sound`: `1`/`adlib`, `sb` or `0`. SWOTL only uses the FM (AdLib) part of a
  Sound Blaster: with the SB setting it writes to the same OPL2 ports and uses
  no digital sound. Both settings therefore sound exactly the same.
- `volume`
- `shader`: `crt-auto` (default), `crt-1080p`, `crt-1440p`, `crt-4k` or `none`.
- `renderer`: `opengl` (default, needed for the shader) or `sdl`.
- `scale`, `aspect`, `integer`, `fullscreen`, `width`, `height`. `scale` and
  `integer` apply when no CRT shader is active.
- `mouse_speed`
- `keyboard_flight`: `stick` (default) or `classic`. See *Keyboard flying* above.
  `keyboard_stick_ramp` and `keyboard_stick_return` set the seconds to full
  deflection and back to the centre.
- `key_repeat_rate`, `key_repeat_delay`: the key repeat while a key is held.
  The defaults are 30 per second and 250 ms.
- `show_fps`

## Building from source

The release build is cross-compiled on Linux with clang, lld, mingw-w64 (built
from source) and a static SDL3 3.2. See `build.sh`. The `tools/` folder has a
headless test runner and a test harness for the CPU core. The CPU core passes
the SingleStepTests 8088 suite, apart from deliberate 286-style behaviour.

## Licences

- SWOTL Enhanced source code: GPL-3.0-or-later, (c) 2026 Zwarteziel. See
  `LICENSE`.
- Nuked-OPL3 (`third_party/opl3.c`, `opl3.h`): LGPL-2.1-or-later, (c) Nuke.YKT.
  See `third_party/LICENSE.Nuked-OPL3`.
- CRT shaders and image adjustments (`third_party/dosbox-staging-shaders`): MIT,
  (c) The DOSBox Staging Team, Hyllian, Tyrells and others. See the README in
  that folder.
- SDL3 (linked statically in the Windows build): zlib licence.
- 8x8 BIOS font (`src/font8x8.inc`): derived from font8x8 by Daniel Hepper
  (public domain).
- Secret Weapons of the Luftwaffe is (c) Lucasfilm Games / LucasArts. No game
  files are included.
