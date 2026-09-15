# BennuGD for MiSTer

A MiSTer core that runs BennuGD 1.x games (Streets of Rage Remake 5.2 first)
on the DE10-Nano's ARM side, with the FPGA supplying video, audio and
controller plumbing. The interpreter is diekleinekuh's BennuGD_libretro,
linked into a small MiSTer-specific host; nothing from RetroArch is used.

## How it fits together

```
MiSTer menu -> Main_MiSTer loads BennuGD.rbf and keeps running
               bennugd-launcherd (user-startup.sh) sees "BennuGD" in /tmp/CORENAME
               and starts /media/fat/bennugd/bennugd --core

HPS: bennugd (hps/frontend)                FPGA: BennuGD.sv (Template + MISTER_FB)
  BennuGD_libretro core, statically linked   scaler reads RGB565 416x240 at 0x22000000
  video  -> /dev/mem @ 0x22000000            alsa.sv carries HPS audio to DAC/HDMI
  audio  -> /dev/MrAudio, 48 kHz             Menu-core NTSC timing, ~59.8 Hz
  input  <- /dev/input/event*
  exit   -> load_core menu.rbf into /dev/MiSTer_cmd
```

## Status

| Phase | What                                      | State |
|-------|-------------------------------------------|-------|
| 1     | BennuGD_libretro cross-compiled for armhf | done  |
| 2     | SorR.dat runs headless on the HPS         | done; after the shadow-surface and non-PIC fixes 4 % of attract-mode frames run over budget |
| 3     | MiSTer frontend (fb, MrAudio, evdev)      | written, fb path tested from ssh |
| 4     | BennuGD.rbf, launch from the menu         | done, SorR on screen 2026-09-15 |
| 5     | Page flip, vsync and OSD-mapped pads via a DDR control block | done 2026-09-15; intro locked to vblank, no drops |
| 6     | Packaging, game picker                    | install.sh only |

## Building

HPS binary, on the Mac (zig 0.14.0 in `~/.local/opt`, `brew install cmake`):

    hps/build.sh              # -> hps/out/bennugd

FPGA, on the Quartus 17.0 Lite box:

    build.bat                 # -> releases/BennuGD_yyyymmdd.rbf

`sys/` is Template_MiSTer as shipped. Do not edit it; replace it wholesale.

## Installing

Copy `hps/out/bennugd` and the three files in `dist/` to `/media/fat/bennugd/`
on the MiSTer and run `sh install.sh <core.rbf>` there. It installs the
launcher daemon into `/media/fat/linux/user-startup.sh` and starts it.
Game data goes under `/media/fat/games/BennuGD/<Game>/` (`tools/deploy.sh
game <folder>` does this without the Windows binaries). No game data ships
with the core.

Controls: map the pad in the MiSTer OSD ("define buttons": A B X Y L R
Select Start); the FPGA passes the result to the game, where SorR sees
them as joystick buttons 0-7. Select+Start together exits to the MiSTer
menu. Loading another core from the OSD stops the game. Direct evdev input
(and keyboard) only works when the frontend is run from ssh without Main.

## Notes

`docs/DESIGN.md` has the verified facts, decisions and measurements.
