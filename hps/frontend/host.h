/* Backend API of the MiSTer BennuGD frontend. main.c drives the libretro
 * core and calls these; each backend lives in its own file. */
#ifndef HOST_H
#define HOST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void host_log(const char *fmt, ...);

/* video.c: fb_phys != 0 maps the DDR3 framebuffers the FPGA scans (RGB565)
 * through /dev/mem; fb_w x fb_h is the fixed geometry used when the core has
 * no control block. dump_every > 0 writes every Nth frame as a PPM into
 * dump_dir (headless testing). */
int  video_init(unsigned long fb_phys, unsigned fb_w, unsigned fb_h,
                const char *dump_dir, int dump_every);
void video_present(const void *data, unsigned w, unsigned h, size_t pitch);
long video_copy_us(void);   /* average copy time (incl. vblank wait) since last call */
bool video_has_vsync(void); /* control block present: page flip at vblank, early frames wait for it */
double video_vblank_hz(void); /* measured at init, 0 without control block */
unsigned video_dropped(void); /* frames dropped since last call */
long video_wait_us_take(void); /* vblank wait time accumulated since last call, then reset */
const volatile uint32_t *video_joystick_words(void);  /* 4 hps_io joystick words, or NULL */
void video_overlay(int on);        /* fps=1: frames/s and worst run ms, top right */
void video_overlay_run_us(long us);
void video_close(void);

/* audio.c: dev is an ALSA device name ("default"); NULL or "none" disables.
 * Input is interleaved S16 stereo at in_rate; writes block, so the ALSA
 * buffer paces the game alongside the frame timer. */
int      audio_init(const char *dev, unsigned in_rate);
void     audio_push(const int16_t *lr, size_t frames);
void     audio_flush(void);
unsigned audio_errors(void);
long     audio_level_ms(void);   /* queued audio at the last flush, -1 unknown */
bool     audio_active(void);   /* true: blocking writes pace the game, skip the frame timer */
void     audio_close(void);

/* input.c: evdev keyboards and gamepads mapped to libretro joypads. */
int     input_init(bool grab);
void    input_poll(void);
int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id);
bool    input_quit_requested(void);
void    input_close(void);

/* prof.c: PC sampler, see the file header. */
int  prof_start(const char *dump_path);
void prof_frame_end(bool keep_this_frame);
void prof_dump(void);

/* launcher.c: the way back to the MiSTer menu. */
void launcher_return_to_menu(void);
void launcher_set_cpu_mhz(int mhz);   /* 400, 800, 1000 or 1200; needs MiSTer Linux 20260912+ */
void launcher_restore_cpu(void);      /* back to 800 MHz if it was changed */

#endif
