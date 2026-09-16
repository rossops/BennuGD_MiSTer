# Save files for testing

These are here so you can reach every part of a game without playing it
through first. They are for testing the core; they will spoil the game.

## SORRv52: everything unlocked

`SORRv52/savegame.sor` is a Streets of Rage Remake 5.2 profile with the
shop bought out, the extra modes, characters and stages unlocked, and a
points balance of 9,999,999. `SORRv52/trophies.sor` goes with it. The
unlock flags come from a save file shared in the SorR community; the
options and control bindings in it were replaced with ones that work with
this core (graphics mode 1x, widescreen on, pad bound to the MiSTer layout
A B X Y L R Select Start), because a save carries the game's options too
and the original ones made the picture pixelated and the pad dead.

### Back up your own save first

SorR keeps your progress, rankings and options in the same file. Copying
this one over yours loses them. On the MiSTer over ssh:

```
cd /media/fat/games/BennuGD/SORRv52
mkdir -p savegame-backup
cp savegame/*.sor savegame-backup/
```

or copy the `savegame` folder somewhere safe over the network share.

### Install

1. Stop the game if it is running (Select+Start on the pad returns to
   the MiSTer menu). The game rewrites its save files on exit, so a copy
   made while it runs can be overwritten.
2. Copy `savegame.sor` and `trophies.sor` into
   `/media/fat/games/BennuGD/SORRv52/savegame/`, replacing the ones there.
3. Launch the game from Other -> BennuGD -> SoRR 5.2. Check the Shop and
   Extra Modes.

If the picture looks wrong or the pad does nothing after loading a
different save than this one, that save's options are the cause: go to
the game's Options, set graphics mode to 1x / normal, and re-bind the pad
under Controls.

### What is in the file, as far as it is known

`savegame.sor` is 1764 bytes, a raw dump of the game's variables, worked
out by diffing three versions. Offsets are bytes, values little-endian
32-bit integers.

| offset | content |
|---|---|
| 0x000 to 0x373 | unlock flags (runs of 0 or 1) and counters; 0x0a8 is the points balance |
| 0x374 to 0x4f3 | options and controls; 0x428 is the graphics mode, 0x49c to 0x4c4 the player 1 bindings (keyboard scancodes below 100, joystick codes from 100 up, 117 to 120 the stick directions), 0x4e4 to 0x4f0 a second binding set |
| 0x5d8 to 0x654 | ranking names |
| 0x658 | profile name |
| 0x670 to 0x6e3 | further flags |

Which flag is which feature has not been mapped; changing one thing in
the game and diffing the file is the way to find out.
