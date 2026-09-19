# BennuGD on MiSTer: design notes

Working notes for the core. The full plan lives in the user's plan file
(`~/.claude/plans/graceful-mapping-dijkstra.md`); this file records what was
verified, what was decided, and the numbers, as the work goes.

## What this is

A MiSTer core whose FPGA half only supplies the console interface. The
DE10-Nano's ARM HPS runs the BennuGD 1.x interpreter under Linux and draws
into a DDR3 framebuffer that the FPGA's scaler picks up. First game is
Streets of Rage Remake 5.2.

```
MiSTer menu -> Main_MiSTer loads BennuGD.rbf and keeps running (OSD, video mode, FIFO)
               (MiSTer.ini: [BennuGD] main=bennugd/bennugd)

HPS                                        FPGA (Template_MiSTer + MISTER_FB)
bennugd (one static binary)                FB_EN=1, RGB565, 320x240, FB_BASE=0x22000000
  BennuGD_libretro core, linked in         ascal scans it -> HDMI / VGA
  video.c  /dev/mem @ 0x22000000           alsa.sv takes HPS audio
  audio.c  /dev/MrAudio, 48 kHz S16 stereo
  input.c  /dev/input/event*
  exit     load_core menu.rbf via /dev/MiSTer_cmd
```

## Verified facts (date, how)

- 2026-09-15. `SorR.dat` header, bytes 8..11 = `10 07 00 00`: DCB version
  0x0710, so BennuGD 1.x. BennuGD2 (SplinterGU) is DCB 0x0800 with a
  different module set and cannot load it. Base is
  diekleinekuh/BennuGD_libretro, pinned as a submodule at 5fba764 (2026-08-14).
- 2026-09-15. Upstream recipe builds unchanged on this Mac with zig 0.14.0:
  `bennugd_libretro.so`, ELF 32-bit ARM EABI5 hard float, 3 warnings, 0 errors.
  The upstream toolchain targets `arm-linux-gnueabihf.2.17`, so the binary
  only needs glibc 2.17 or newer. No musl or static-libc games needed.
- 2026-09-15. The core reports `need_fullpath = true`, extensions `dat|dcb`,
  RGB565, 44100 Hz, 60 fps default, one BennuGD frame per `retro_run()`
  (libco). It uses SET_VARIABLES for options and SHUTDOWN when the game exits.
- 2026-09-15. `hps/build.sh` produces one ARM executable, `hps/out/bennugd`
  (core linked statically, 8.8 MB with debug info, no frontend warnings).
  Not yet run on the device.
- MiSTer framework (Template sys/emu_ports.vh): `FB_FORMAT[2:0]=100` 16bpp,
  `[3]=0` 565. Bit 4 is documented as 0=RGB 1=BGR, but with 0 a standard
  little-endian RGB565 buffer showed red and blue swapped on screen
  (2026-09-15 capture); 240mp sets the bit for its XRGB8888 buffer too.
  So `FB_FORMAT = 5'b10100`. `alsa.sv` is built unless `MISTER_DISABLE_ALSA`.
  ascal's own buffers start at 0x20000000; Main_MiSTer's Linux fb region is
  0x22000000 and up, free while our binary has replaced Main.
- MiSTer Linux release 20260907 (kernel 6.18.38) broke `mmap()` on
  `/dev/fb0`. We map `/dev/mem` at the physical address instead, same as
  Main_MiSTer itself.

## Decisions

- Keep the libretro API as the boundary. Our frontend is a small host that
  links the core statically (`hps/CMakeLists.txt` sets
  `TARGET_SUPPORTS_SHARED_LIBS` false before adding the submodule, so the
  upstream `libretro/CMakeLists.txt` picks STATIC). Upstream stays unpatched.
- Cross-compile on the Mac with zig 0.14.0 and cmake, `-mcpu=cortex_a9
  -mfpu=neon`, glibc 2.17 floor. Build tree on local disk
  (`~/.cache/bennugd-mister`), the SMB share is far too slow for it.
- Audio goes straight to `/dev/MrAudio` with a linear resampler from the
  core's rate to 48 kHz. No libasound.
- Fixed 320x240 framebuffer until the Phase 5 control block lets the HPS
  change geometry.
- Native 15 kHz analog output (a DDR3 reader like 3s-mister-arm) is deferred.

## Device survey (Phase 0, 2026-09-15, MiSTer at 192.168.68.246)

- Kernel 6.18.38-MiSTer (built 2026-09-12), glibc 2.31, Main_MiSTer dated
  2026-09-15.
- Linux RAM: `mem=511M`, System RAM 0x00000000-0x1fefffff, `free` shows
  491 MB total, 449 MB available with Main idle. Everything from 0x20000000
  up belongs to the FPGA side, as assumed.
- `/etc/asound.conf`: default -> plug -> rate 48000 S16_LE -> file
  `/dev/MrAudio` (raw) -> hw card 0. Writing raw S16_LE 48 kHz straight to
  `/dev/MrAudio` is exactly what ALSA does for every other app.
- `/dev/mem` is `crw-r-----` root:kmem; we run as root. `/dev/fb0` exists
  but is not used. `/dev/MiSTer_cmd` is a FIFO.
- Inputs seen: a Logitech Dual Action pad (event0) and "MiSTer virtual
  input" (event1, a keyboard-class device Main synthesises).
- Lesson: never run `/media/fat/MiSTer` with arguments over ssh to "check
  its version". It ignores flags and starts a second Main instance.

## Phase 4 on hardware (2026-09-15): SorR on the capture card

`BennuGD_20260915.rbf` built on the Windows box: 17% of the ALMs, all
clocks with positive slack, the only critical warning is the framework's
own ascal `mode` port width that every Template core shows. The user
confirmed the game running on screen through the capture card.

**Launch model: Main_MiSTer stays running.** `bennugd-launcherd.sh`
(started at boot from `/media/fat/linux/user-startup.sh`) polls
`/tmp/CORENAME` and runs `bennugd --core` when it reads `BennuGD`, kills
it when the name changes. Exit from inside the game is one line into
`/dev/MiSTer_cmd` (`load_core /media/fat/menu.rbf`). Input devices are
not grabbed in this mode so Main's OSD keeps working.

**The `main=` handoff was tried first and abandoned.** It hands the FPGA
to us before Main programs the HDMI mode and the scaler, so the screen
stays dead; the frontend also had to release the framework reset itself
and reprogram the FPGA with menu.rbf on exit (Main re-execs anything
named in `main=` whenever it starts with that core loaded). A port of
Main's fpga_io.cpp loader made that work, but it was a lot of fragile
code for a black screen. 3s-mister-arm gets away with `main=` only
because its wrapper is a modified Main. Lesson: if Main is not running,
nothing programs the video path.

Device hygiene learned the hard way: a stray Main instance recreates
`/dev/MiSTer_cmd`, leaving the real Main reading a deleted inode; only a
reboot fixes that. Never scp over the running binary (ETXTBSY, silently
keeps the old one). The MiSTer's `/tmp` is a 246 MB tmpfs.

## Evening of 2026-09-15: colours, input, sound, speed

- **Colours** were red/blue swapped: `FB_FORMAT` bit 4 must be set for a
  little-endian RGB565 buffer (see the verified facts). Fixed in the
  second rbf of the day.
- **Input was dead under the daemon model.** Main_MiSTer grabs every evdev
  device (`input.cpp`, `grabbed = 1` by default, EVIOCGRAB in the poll
  loop), so a second reader gets nothing. The fix is the one the original
  plan wanted anyway: the FPGA provides the controllers. hps_io's four
  joystick words, mapped by the user in the OSD ("define buttons" now
  lists A B X Y L R Select Start from the CONF_STR J line), are written by
  `rtl/bennugd_ctl.sv` into the DDR control block every vblank and read
  by `input.c`. evdev stays for ssh runs without Main.
- **Control block** (`rtl/bennugd_ctl.sv`, 64 bytes at 0x23F00000): the
  HPS publishes the buffer to show (0x22000000 or 0x22100000) and its
  geometry; the FPGA publishes a vblank counter and the joystick words.
  `video.c` renders into the back buffer, flips, and waits for the
  counter: page flip with vsync, no more fixed 416x240 and no tearing.
  Detected at start by watching the counter, so the frontend still works
  with the first rbf (single buffer, evdev, no sync).
- **Sound** moved to ALSA `default` via dlopen (plug -> rate -> tee into
  /dev/MrAudio -> dummy card 0). `/dev/MrAudio` on its own never blocks
  (1 MB accepted in 55 ms), so it gives no flow control at all. With
  blocking ALSA writes and a 100 ms buffer, the audio is the clock: the
  game runs as fast as the buffer drains and catches up after a slow
  frame. `audio_err` in the stats counts underruns.
- **Phase 5 numbers, intro sequence, control-block rbf (21:31):** frame
  plus audio p50 16.77 ms (locked to the 59.84 Hz vblank), 0 dropped
  frames, audio underruns only during the 3.7 s load and none after. The
  pacing chain that got there: ALSA buffer 100 ms with a 4 ms period (the
  default 25 ms period made the game run in bursts and drop 6 % of frames
  at the flip); when a frame arrives before the FPGA has latched the
  previous one, wait for that vblank (bounded to one frame) instead of
  dropping; and open ALSA at 735 frames per vblank (43982 Hz) so the
  audio clock and the vblank clock cannot drift apart. `fbcopy` in the
  stats now includes that wait, so it reads 6-7 ms rather than 2.5.
- **Profile of the slow frames (2026-09-15 21:42, `prof=` sampler,
  `tools/profsym.py`):** BlitNtoN 29 %, instance_go 23.5 %, memcpy 12 %,
  gr_unlock_screen 7 %, the draw_hspan_* sprite routines 12 % together,
  vorbis 2 %. BlitNtoN is SDL's generic converting blit: the libretro SDL
  shim's ListModes only admitted 32 bpp, so SDL gave the 16 bpp game a
  shadow surface and converted the whole frame on every flip, for nothing
  (the core hands the 16 bpp shadow to the frontend anyway). Fixed by
  listing every depth as native in `SDL_LIBRETROvideo.c`; the frontend
  logs the surface format at start (`sdl:` line) so this cannot regress
  silently. The change lives in the submodule working tree and in
  `hps/patches/0001-sdl-libretro-list-all-depths.patch`; it should go
  upstream as a pull request, or the submodule should point at a fork.
- **Non-PIC build (22:00):** upstream compiles everything `-fPIC` for its
  shared core; linked into an executable that only taxes the interpreter
  (every global access in the VM loop goes through the GOT on ARM32).
  `hps/CMakeLists.txt` switches `POSITION_INDEPENDENT_CODE` off for every
  submodule target. Frames over budget in the first three minutes of
  attract mode: 1512/10801 (14.0 %) before, 292/7201 (4.1 %) after; the
  interpreter's median frame 15.7 ms -> 10-11 ms (`run` in the stats now
  excludes the vblank wait). In what is left, instance_go is 55 %, libc
  stdio + memcpy about 15 % (SorR streams sound effects from the .dat
  during play), gr_unlock_screen 5 %, sprite spans under 10 %.
  Caller attribution by LR (`tools/profsym.py`) is only meaningful for
  leaf functions.
- **Speed: what remains.** In the intro the interpreter takes
  8-11 ms per frame; in busy levels and the busy attract stages 12-13 ms
  median with a 99th percentile of 23-25 ms, and stretches above 16.7 ms
  are audible as stutter and visible as slowdown. Numbers from
  `run p50/p99` in the stats lines. The uncached framebuffer copy is a
  fixed 2.5 ms of that (identical with and without O_SYNC). The core is
  built -O3 -mcpu=cortex_a9 -mfpu=neon. There is no cpufreq on this
  kernel, so 3s-mister-arm's sysfs overclock does not apply here.
  `-flto` was tried and dropped: the binary dies silently right after
  `retro_init` (libco or something else miscompiled under LTO; not worth
  chasing). The sound-load hitches are cold SD reads out of the 320 MB
  .dat (the core's filesystem layer caches names once, it does not scan
  per open), so the frontend now pre-reads the game file into the page
  cache from a nice-19 child after start; the MiSTer has ~450 MB free.
- **The user's heavy level, profiled (00:00-00:15):** 44 % of frames over
  budget there, interpreter median 16.6 ms. Split: instance_go 28 %,
  draw_span_16to16_translucent 15 % (scaled/rotated translucent sprites,
  table-based blend `ghost1[tex] + ghost2[dst]`, two 128 KB tables per
  pixel), my uncached framebuffer copy 12 %, gr_unlock_screen 7 %, the
  other 16-bit spans about 10 %.
  - gr_unlock_screen: SorR sets `scale_resolution` to its own 416x240, so
    BennuGD gathered the frame pixel by pixel through identity tables
    every frame. `hps/patches/0002` adds a row-copy fast path for the
    equal-size unrotated case. Median in the level after it: 4.6-4.9 ms,
    over-budget frames 3.8 % (was 44 %; some of the difference is where
    in the level the samples fell, but the unlock cost is gone).
  - The framebuffer copy stays at 2.5 ms with NEON 16-byte stores too: the
    uncached mapping is bus-bound, not instruction-bound. Left as is.
  - Audio: the buffer is primed with silence at start and after every
    underrun, and `audio.c` nudges one frame per flush to hold the queued
    level near the primed level (`level` in the stats). Needed because
    with vsync the game produces audio exactly as fast as it is consumed.
- **The second core was not free (00:50).** `top` on the device: Main_MiSTer
  at ~90 % of a core, pinned to CPU 1, busy-polling the FPGA the whole
  time a non-menu core is loaded; our game thread, the copier thread and
  the pre-read shared what was left. Two changes: the daemon renices Main
  to 10 while the BennuGD core is loaded (back to 0 when it unloads), and
  the frontend pins the game thread to CPU 0 and the copier thread to
  CPU 1, where it only has to beat Main's polling. The uncached 2.5 ms
  framebuffer copy now runs on the copier (video.c: video_present copies
  into a cached staging buffer and hands over; the copier does the DDR
  copy, the flip and the vblank wait). After that the frame total sat on
  the vblank period with a flat underrun count in the same level.
  Remaining levers: the translucent span blend (arithmetic instead of the
  two tables, NEON where the fetch is linear), and the interpreter loop.
  Caller attribution by LR in tools/profsym.py is not trustworthy for
  non-leaf functions; ignore that section unless the callee is a leaf.
- **Scene transitions (01:30):** a 350 ms level-load frame drained the
  audio buffer, and from then on the game was paced by the copier's
  vblank wait instead of the audio clock. The kernel's dummy card runs
  about 0.7 % fast against the rate asked for, the one-frame-per-flush
  regulation covers 0.14 %, so the buffer bled to an underrun every few
  seconds. Fix: the copier never holds the game back. Two cached staging
  buffers; the game always writes the one the copier is not reading, a
  newer frame replaces a waiting one, and the copier shows the newest at
  each vblank (`dropped` in the stats counts replacements, about 0.5 %).
  Audio is the only clock. Six minutes of play across transitions after
  that: 1 underrun, queued level 62-89 ms. The user confirmed the
  transition stutter gone.
- **Crash after the first boss (02:10):** `Unhandled fault: alignment
  exception (0x011) at 0x0183ad50` in dmesg, the frontend gone, black
  screen. The address is in the heap (the fb mapping is at 0xb6386000),
  so it is the core's code, on an access the kernel's alignment fixup
  does not emulate. The frontend now logs pc/lr on SIGSEGV/SIGBUS/SIGILL
  (`CRASH:` line in bennugd.log, symbolise with the symtab) and the
  daemon restarts a frontend that died with the core still loaded. Rows
  in DDR are 64-byte aligned (stride published in the control block).
  Second crash gave the pc: `adler32_neon` in zlib-ng, instruction
  `f427623d` = VLD1.8 {d6-d9} with a `:256` alignment qualifier, on a
  16-byte-aligned buffer. zlib-ng's NEON adler32 assumes 32-byte
  alignment it is not given. Fixed by building zlib-ng with
  `WITH_NEON=OFF` (hps/CMakeLists.txt); the C checksum costs nothing
  noticeable. Worth reporting to zlib-ng / BennuGD_libretro.
- **Double-speed gameplay after the level 2 truck scene (02:30):** the
  core logged `av info 416x240 @ 120 fps`. SorR's `mod/system.txt` mode
  BORDERLESS_SYNC (the only mode that renders at native 416x240; AUTO and
  BORDERLESS render 832x480, a 2x scale the interpreter would pay four
  times over) calls `set_fps(120)` and relies on the PC's 60 Hz vsync to
  pace the game. The libretro core only ever raises its reported rate and
  BennuGD's own limiter is off while the two match, so the game ran 120
  logic frames a second with the audio per frame halved (sound normal,
  play doubled). `hps/patches/0003` stops the core reporting more than
  the frontend's refresh rate; the audio upload already follows the
  reported rate, so the game runs 60 logic frames a second, as on a
  vsynced PC. Keep BORDERLESS_SYNC in system.txt.
- **The sampler itself causes stutter (01:05).** With `prof=` on, a light
  scene (2 ms frames) still lost about five audio periods a second and the
  queued level kept collapsing; the same scene without it held a flat
  underrun count at 72-89 ms queued. The 1 kHz SIGPROF interrupts the
  blocking ALSA write and the pacing waits. So the daemon no longer
  passes `prof=`; enable it in bennugd.cfg for a profiling session only,
  and treat the audio counters from such a session as meaningless. The
  in-process sampler (`prof=<file>` plus `tools/profsym.py`) makes each
  experiment a three-minute measurement on the real hardware.

## Phase 4 project (written 2026-09-15)

`BennuGD.sv` is Template's emu skeleton with the Menu core's NTSC timing
(PLL 100/20 MHz from Menu_MiSTer/rtl/pll) and the MISTER_FB constants:
RGB565, 416x240, stride 832, base 0x22000000. RGB outputs are zero, audio
outputs zero with AUDIO_S=1, DDRAM idle. Verilator lint (5.050) is clean
with stubs for pll and hps_io. `BennuGD.qsf` is Template.qsf plus
MISTER_FB=1, Lite edition, and the build_id pre-flow script; deviations are
listed at the top of the file.

## Game selection (Phase 6, 2026-09-15 evening)

Main_MiSTer's own menu does the picking. `_Other/_BennuGD/` (the core
browser only lists folders whose name starts with `_`) holds the rbf
and one `.mgl` per game (`<rbf>_Other/_BennuGD/BennuGD</rbf>` resolves to
the newest `BennuGD_*.rbf` there; `<file type="s" index="0" path=...>`
mounts the .dat in the core's `S0` slot, which moves no data, unlike an
`F` slot that would push 320 MB over SPI). With `log_file_entry=1` in the
`[BennuGD]` ini section Main writes the selected path to `/tmp/FULLPATH`;
the daemon accepts it when it is newer than `/tmp/CORENAME` and under the
games folder, and restarts the frontend when it changes (the OSD's "Load
game" switches games). Loading the bare rbf falls back to `game=` in
bennugd.cfg. Relative `.mgl` paths are under `games/BennuGD/`.

Two traps met on the way: Main records the path relative to the games
folder (`BennuGD/SORRv52/SorR.dat`), so the daemon resolves it against
the games folder and the card root; and busybox `pidof -x` does not see
shell scripts, so an installer that "restarted" the daemon left the old
ones running and each kept launching a frontend. The daemon now kills
any other instance at start and every stray frontend before launching,
keys restarts on the resolved game path only, and logs to
`/media/fat/bennugd/launcherd.log`. Verified: `load_core` of the .mgl
from the menu gives exactly one daemon and one frontend on the selected
game.

## Numbers

Phase 2, 2026-09-15, headless run of SorR.dat on the stock 800 MHz HPS
while the Menu core was loaded (audio off, PPM dump every 120 frames,
1800 frames). Frame time is `retro_run()` plus dump work:

| frames    | p50     | p99      | max        | RSS     |
|-----------|---------|----------|------------|---------|
| 1-600     | 8.3 ms  | 176.7 ms | 4051 ms    | 25.5 MB |
| 601-1200  | 9.3 ms  | 20.1 ms  | 189.7 ms   | 25.6 MB |
| 1201-1800 | 8.3 ms  | 17.9 ms  | 190.4 ms   | 25.6 MB |

The first block includes the 4 s load. The ~190 ms spikes line up with
the PPM dumps (every 120 frames, 300 kB each to the SD card). So the
interpreter alone is well inside the 16.7 ms budget on the intro; a level
with many sprites is still to be measured. Memory is a non-issue.

Phase 3 check, same day: with the framebuffer mapped through `/dev/mem`
(`O_SYNC`, so uncached device memory) and every frame copied in, 1300
frames, no dumps:

| frames    | p50     | p99     | max     |
|-----------|---------|---------|---------|
| 601-1200  | 11.6 ms | 13.1 ms | 20.2 ms |

So the 200 kB uncached copy costs about 2.5 ms per frame. Fine for now,
and the first thing Phase 5 should attack (map without `O_SYNC` and see if
the kernel gives write-combining; NEON stores; copy only the rows that
changed).

Surface: the core starts at 320x200, then SorR switches to 416x240
(its widescreen mode). The fixed framebuffer is therefore 416x240 and
narrower surfaces are centred (`FB_W`/`FB_H` in main.c, `FB_WIDTH`/
`FB_HEIGHT` in BennuGD.sv must match).

## 2026-09-16 afternoon: the interpreter and the translucent blend

- **Tooling.** `tools/proflines.py <binary> <prof.bin> <function>` buckets
  the sampler's pcs inside one function by source line (the binary already
  carries DWARF line tables; Apple's `objdump -d -l` reads the ARM ELF, but
  `--disassemble-symbols` stops at the first inline jump table, so the
  script takes the range from `nm` and disassembles by address). Also:
  the sampler counts the copier thread too (`copier_main` shows up at
  10-13 %); ignore it, it is not on the game thread.
- **Attract mode is not a repeatable workload.** The game's sequencing is
  wall-clock based (SDL_GetTicks), so a load hitch shifts every later
  scene, and a restart may resume into the last level instead of the intro
  (SorR keeps a continue state). Two runs of the same binary five minutes
  apart had different scenes at the same frame index; only the first
  ~3600 frames (the intro) line up. A/B numbers from attract mode are
  therefore indicative only; the user's heavy level is the real test.
- **Interpreter loop overhead (line profile, baseline binary, resumed
  level):** of instance_go's own samples, 15 % sat on
  `if (trace_sentence != -1)`, 7 % on `if (debug > 0)`, 13 % on the
  switch dispatch, 8 % on the status check and 5 % on `while (!must_exit)`.
  Each of those globals was read through the GOT (literal -> GOT entry ->
  value, three dependent loads): zig cc refuses `-fno-pic` for this
  target ("the selected target requires position independent code"), and
  `POSITION_INDEPENDENT_CODE OFF` only stops CMake adding `-fPIC`.
  `-fdirect-access-external-data` is not honoured through zig either.
  - Patch 0004: the trace/debug checks are compiled out under
    `LIBRETRO_CORE` (nothing in the libretro build ever sets `debug`, and
    a `debug` statement in a game would spin forever waiting for a console
    that does not exist), and `globaldata`, `localdata`, `debug` get a
    hidden-visibility attribute in bgdrtm.h (`BGDRTM_HOT_GLOBAL`).
  - `hps/CMakeLists.txt` adds `-fvisibility=hidden` for everything, so
    globals defined in the same file are addressed pc-relative
    (`must_exit`, `ghost1/2`, the stack). GOT loads inside instance_go:
    43 -> 27; the loop head is now must_exit, status, dispatch.
- **Translucent sprites (patch 0005).** `draw_span_16to16_translucent`
  (scaled/rotated) was 18 % of the slow-frame samples in the resumed level
  and `draw_hspan_16to16_translucent` another 3.5 %. The blend is
  `ghost1[tex] + ghost2[dst]`, two 128 KB tables that miss L1 on nearly
  every pixel. When the tables are alpha tables (`gr_alpha16`) or the 50 %
  `colorghost`, the same result is per component `((c * f) >> 8)` summed,
  so g_blit.c now computes it in registers (r and b packed in one word,
  13-bit products cannot overlap; the packed add cannot carry between
  fields because floor(c*f1/256) + floor(c*f2/256) <= c). The factor comes
  from a new `gr_alpha16_factor()` that mirrors the table quantisation
  (`ALPHA_STEPS`, default 0 -> a single 50 % table). Verified bit-exact
  against the table formula for every step count and 158k random pixel
  pairs, and gated on the surface being RGB565; a per-graph `blend_table`
  still uses the tables.
- **Result so far.** Both binaries run 5+ minutes in attract with no crash.
  Profile shares moved as expected (the translucent span is gone from the
  top of the list, instance_go self time is 45-49 % of what is left). In
  the only aligned window (the intro, frames 600-3600) run p50 8.1 -> 7.8
  ms and p99 20.3 -> 17.4 ms; the heavy level has not been re-measured
  (needs a player). What remains in instance_go is the per-opcode stack
  traffic through `r->stack_ptr` in memory, the status check and the
  dispatch itself; the next step there would be caching the stack pointer
  in a register across the switch, which touches every opcode.
- **fps=1 overlay (throwaway diagnostics).** video.c stamps two numbers
  into the staged frame's top right: frames presented in the last second
  and the longest interpreter frame of that second in ms. With it on, the
  PPM dump (`dump_dir=` / `dump_every=`) captures the staged frame so the
  overlay can be checked from a dump; reading the DDR buffer back through
  `/dev/mem` with dd returns 0 bytes on this kernel.

## 2026-09-18: native 240p video (rtl/bennugd_video.sv)

- **Why.** With MISTER_FB the picture only existed inside the scaler: the
  core's own VGA outputs were black, so `direct_video=1` (issue #1) and
  the analog board showed nothing. Now the FPGA reads the RGB565 frame
  back out of DDR3 a row ahead of the beam and drives VGA_R/G/B/HS/VS/DE
  with a 240p timing. HDMI goes through the scaler like every other core
  (FB_EN is off; an OSD switch "HDMI picture: HPS framebuffer" turns the
  old path back on as a fallback), analog and direct_video get the same
  timing directly. No HPS change: the control block already carries
  width/height/stride/present and the vblank counter keeps its meaning.
- **Timing.** CLK_VIDEO and DDRAM_CLK are now the 100 MHz PLL output. A
  line is 6360 cycles (63.6 us, 15.72 kHz), a frame 262 lines (60.0 Hz;
  was 59.84 with the Menu timing, so the audio rate the frontend derives
  from the measured vblank moves with it). HSync is the first 470 cycles,
  VSync lines 245-247, rows 0-239 carry the picture. The pixel clock is
  CE_PIXEL every `div` cycles with `div` from a ladder on the frame width
  (<=256: 20, <=320: 16, <=352: 14, <=416: 12, <=512: 10, <=640: 8,
  <=832: 6, else 4), so the active area is always about 50 us wide: a
  4:3 game fills a 4:3 CRT, SorR's 416 px does too (squeezed, as on any
  4:3 set; the OSD aspect option still governs the scaler). The active
  window is centred in the nominal 51.7 us visible span; frames shorter
  than 240 rows are centred vertically; taller ones (SorR's 832x480
  modes) show every other row. With hps_io's forced_scandoubler every
  horizontal figure halves, the frame has 524 lines and each row is shown
  twice (the Menu core's approach); the ladder uses even dividers so the
  halves stay integers.
- **Rows.** At the start of each output line bennugd_video asks
  bennugd_ctl for the next line's row into the other half of a two-row
  line buffer; bennugd_ctl (still the only DDRAM master) reads width/4
  beats in one burst (208 beats for 832 px, well inside the 10 us before
  the active area) and queues the vblank control-block traffic behind
  any fetch. The buffer is four 16-bit banks written 64 bits at a time, so
  each pixel is one read of the bank its x selects; the address is
  always the word of the pixel after the one starting at the current
  CE, which keeps the pipeline right down to 2 cycles per pixel.
  Bandwidth: 13 MB/s progressive, 26 scandoubled.
- **Verification.** `tools/sim.sh` runs `rtl/tb/tb_video.sv` under
  iverilog against a DDRAM model whose pixel (x, y) is {y, x}: every
  active pixel of frames 3 and 4 is compared, plus the active size and
  the line/frame periods, for 416x240, 320x200, 832x480 and 416x240
  scandoubled (about 80 s). Geometry from the control block takes two
  frames to reach the display (latched at the top of vblank, read at
  vsync), so a mode change has one wrong frame; harmless. verilator lint
  of `emu` is clean for our files (remaining messages are hps_io's PS/2
  registers under an ifdef, which Quartus accepts).
- **On the hardware (18:40).** First Quartus build failed timing on the
  x0 path (width ladder + multiply + centre in one cycle, -2.3 ns at
  100 MHz); pipelined into three registers, and the row-address multiply
  moved to request time. The first rbf was tried anyway: the frontend
  found the control block at 60 Hz and the user reported the HDMI picture
  through the scaler "looks really good". Also fixed on the way: build.bat
  ran Quartus in C:\Windows when launched from the UNC path (cmd.exe
  cannot cd to \\server\share), now pushd's its own directory. The
  installer no longer forces direct_video=0 and removes the line it once
  added.
- **Vblank rate measured wrong (19:00, from the issue #1 follow-up).** The
  reporter's logs showed the vblank at 60.4-61.1 Hz on a 59.83 Hz timing,
  different on every launch, the audio opened at 735x that, the game
  outran the display (5 % dropped frames) and audio underran. video.c
  started its clock at a random phase, so 30 counted edges spanned 29-30
  periods: up to 3 % high. Now the clock starts on an edge and counts 60
  of them; three starts read 60.00/60.02/60.03 Hz on the new core, where
  the old code read 60.24 and 60.32.

## 2026-09-19: the game over CIFS (issue #1, second round)

- The reporter (alpha-20260918, SorR.dat on a CIFS mount) still saw
  brief stutters in busy sections, `fbcopy` at 8 ms for most of the run,
  25-36 dropped frames per 600 and a climbing `audio_err`. Reproduced
  here with the .dat on the NAS (macOS SMB server, wired gigabit,
  `mount -t cifs ... -o guest,vers=3.0`; the daemon had to be bypassed
  because Main rewrites /tmp/FULLPATH from the mounted image, so the
  frontend was started by hand with `--core game=/tmp/nas/...`).
  Result: for the first 80 s, while the 4 MB/s pre-read is still pulling
  the 320 MB file, every window shows 3-8 drops, p99 20-34 ms and
  audio_err ticking up (each read the game makes before the pre-read gets
  there is a network round trip); after that the run is identical to the
  SD card, 0 drops, fbcopy 2.3 ms. So CIFS explains the reporter's stalls
  in the first minute or two but not the 8 ms fbcopy (a plain memory
  copy on CPU 1): that needs something else eating that CPU, most likely
  SMB signing/encryption or a USB Wi-Fi adapter; asked for their mount
  options and `top`.
- Change: the pre-read rate is now 16 MB/s when the game sits on a
  network filesystem (cifs/smb3/nfs/fuse, found from the longest
  matching mount point in /proc/mounts; `statfs()` returned garbage for
  the CIFS path in this zig/glibc-2.17 build, f_type 0) and stays 4 MB/s
  on local storage where the concern was memory bandwidth, not latency.
  `preread=<MB/s>` in bennugd.cfg overrides, 0 disables.
- Issue #2's log, read the same way: their slow "club screen" session ran
  at 640x480 (graphics mode 2x: fbcopy 10-11 ms, run p50 13-14 ms, 40-57
  drops per 600); at 416x240 on the fixed frontend they have ~0 drops and
  the heavy scenes sit at 15-17 ms per frame, i.e. the interpreter.

