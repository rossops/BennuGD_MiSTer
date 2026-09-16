/* Audio through ALSA's "default" device. On MiSTer that chain is
 * plug -> rate 48 kHz -> file tee into /dev/MrAudio (the FPGA's alsa.sv
 * ring) -> hw card 0, a timer-driven dummy card. The dummy card gives the
 * blocking flow control that /dev/MrAudio itself lacks (its writes never
 * block, so late audio just becomes a gap). libasound is loaded at run
 * time so the cross build needs no ALSA headers or import library. */
#define _GNU_SOURCE
#include "host.h"
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>

#define ACC_MAX     16384u    /* input frames buffered between flushes */
#define BUFFER_MS   100u      /* ALSA buffer: rides over the 20-30 ms frame spikes seen in play */
#define PERIOD_MS   4u        /* small period: writes block a few ms at a time, so the game paces
                                 frame by frame instead of in bursts (bursts drop frames at the flip) */

typedef void snd_pcm_t;
typedef void snd_pcm_hw_params_t;
static int   (*p_open)(snd_pcm_t **, const char *, int, int);
static int   (*p_hw_malloc)(snd_pcm_hw_params_t **);
static void  (*p_hw_free)(snd_pcm_hw_params_t *);
static int   (*p_hw_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
static int   (*p_hw_resample)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
static int   (*p_hw_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
static int   (*p_hw_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
static int   (*p_hw_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
static int   (*p_hw_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *, int *);
static int   (*p_hw_buffer_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned long *);
static int   (*p_hw_period_near)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned long *, int *);
static int   (*p_hw_apply)(snd_pcm_t *, snd_pcm_hw_params_t *);
static long  (*p_writei)(snd_pcm_t *, const void *, unsigned long);
static int   (*p_delay)(snd_pcm_t *, long *);
static int   (*p_recover)(snd_pcm_t *, int, int);
static int   (*p_close)(snd_pcm_t *);
static const char *(*p_strerror)(int);

static void     *lib;
static snd_pcm_t *pcm;
static int16_t   acc[ACC_MAX * 2];
static unsigned  acc_n;
static unsigned  errors;
static unsigned  prefill_frames;
static long      level_frames = -1;   /* queued frames at the last flush, for the stats */
static int       trim;                /* +1 duplicate / -1 drop one frame per flush: rate regulation */

/* With vsync the game produces audio exactly as fast as it is consumed, so
 * the buffer level never rises on its own: it stays where it started. Start
 * it (and restart it after an underrun) most of the way up with silence. */
static void prefill(void)
{
    static int16_t silence[8192 * 2];
    unsigned left = prefill_frames;
    while (left) {
        unsigned n = left < 8192 ? left : 8192;
        long w = p_writei(pcm, silence, n);
        if (w < 0) break;
        left -= (unsigned)w;
    }
}

int audio_init(const char *dev, unsigned rate)
{
    if (!dev || !strcmp(dev, "none")) { host_log("audio: disabled"); return 0; }
    lib = dlopen("libasound.so.2", RTLD_NOW);
    if (!lib) { host_log("audio: libasound.so.2 not found"); return -1; }
    p_open          = dlsym(lib, "snd_pcm_open");
    p_hw_malloc     = dlsym(lib, "snd_pcm_hw_params_malloc");
    p_hw_free       = dlsym(lib, "snd_pcm_hw_params_free");
    p_hw_any        = dlsym(lib, "snd_pcm_hw_params_any");
    p_hw_resample   = dlsym(lib, "snd_pcm_hw_params_set_rate_resample");
    p_hw_access     = dlsym(lib, "snd_pcm_hw_params_set_access");
    p_hw_format     = dlsym(lib, "snd_pcm_hw_params_set_format");
    p_hw_channels   = dlsym(lib, "snd_pcm_hw_params_set_channels");
    p_hw_rate_near  = dlsym(lib, "snd_pcm_hw_params_set_rate_near");
    p_hw_buffer_near = dlsym(lib, "snd_pcm_hw_params_set_buffer_size_near");
    p_hw_period_near = dlsym(lib, "snd_pcm_hw_params_set_period_size_near");
    p_hw_apply      = dlsym(lib, "snd_pcm_hw_params");
    p_writei        = dlsym(lib, "snd_pcm_writei");
    p_delay         = dlsym(lib, "snd_pcm_delay");
    p_recover       = dlsym(lib, "snd_pcm_recover");
    p_close         = dlsym(lib, "snd_pcm_close");
    p_strerror      = dlsym(lib, "snd_strerror");
    if (!p_open || !p_hw_malloc || !p_hw_free || !p_hw_any || !p_hw_resample || !p_hw_access || !p_hw_format ||
        !p_hw_channels || !p_hw_rate_near || !p_hw_buffer_near || !p_hw_period_near || !p_hw_apply ||
        !p_writei || !p_delay || !p_recover || !p_close || !p_strerror) {
        host_log("audio: libasound symbols missing"); return -1;
    }
    if (!rate) rate = 44100;
    int err = p_open(&pcm, dev, 0 /* SND_PCM_STREAM_PLAYBACK */, 0);
    if (err < 0) { host_log("audio: open %s: %s", dev, p_strerror(err)); pcm = NULL; return -1; }
    snd_pcm_hw_params_t *hw;
    unsigned r = rate; int dir = 0;
    unsigned long buf = (unsigned long)rate * BUFFER_MS / 1000, per = (unsigned long)rate * PERIOD_MS / 1000;
    if (p_hw_malloc(&hw) < 0) { p_close(pcm); pcm = NULL; return -1; }
retry:
    if ((err = p_hw_any(pcm, hw)) < 0 ||
        (err = p_hw_resample(pcm, hw, 1)) < 0 ||
        (err = p_hw_access(pcm, hw, 3 /* RW_INTERLEAVED */)) < 0 ||
        (err = p_hw_format(pcm, hw, 2 /* S16_LE */)) < 0 ||
        (err = p_hw_channels(pcm, hw, 2)) < 0 ||
        (err = p_hw_rate_near(pcm, hw, &r, &dir)) < 0 ||
        (err = p_hw_buffer_near(pcm, hw, &buf)) < 0 ||
        (err = p_hw_period_near(pcm, hw, &per, &dir)) < 0 ||
        (err = p_hw_apply(pcm, hw)) < 0) {
        /* seen once right after boot (EINVAL): the card was not ready yet */
        static int attempts;
        host_log("audio: hw params: %s (attempt %d)", p_strerror(err), attempts + 1);
        if (++attempts < 5) { sleep(1); r = rate; buf = (unsigned long)rate * BUFFER_MS / 1000; per = (unsigned long)rate * PERIOD_MS / 1000; goto retry; }
        p_hw_free(hw); p_close(pcm); pcm = NULL; return -1;
    }
    p_hw_free(hw);
    prefill_frames = (unsigned)(buf - 2 * per);
    prefill();
    host_log("audio: %s, %u Hz S16 stereo, buffer %lu frames (%lu ms), period %lu frames, primed %u",
             dev, r, buf, buf * 1000 / r, per, prefill_frames);
    return 0;
}

void audio_push(const int16_t *lr, size_t frames)
{
    if (!pcm) return;
    if (acc_n + frames > ACC_MAX) { errors++; frames = ACC_MAX - acc_n; }
    memcpy(acc + acc_n * 2, lr, frames * 4);
    acc_n += frames;
}

/* Keep the queued level near the primed level. The game is paced by the
 * FPGA's vblank and the sink by the dummy card's timer, and the rate we
 * opened with is only an estimate of that ratio: left alone the level
 * drifts until it underruns or blocks. One frame duplicated or dropped
 * per flush is a 0.14 % nudge, inaudible, and enough to hold it. */
static void regulate(void)
{
    long d;
    if (p_delay(pcm, &d) < 0) { level_frames = -1; return; }
    level_frames = d;
    long target = (long)prefill_frames, band = 441;   /* 10 ms */
    trim = d < target - band ? 1 : d > target + band ? -1 : 0;
}

void audio_flush(void)
{
    if (!pcm || !acc_n) return;
    regulate();
    if (trim > 0 && acc_n < ACC_MAX) { acc[acc_n * 2] = acc[(acc_n - 1) * 2]; acc[acc_n * 2 + 1] = acc[(acc_n - 1) * 2 + 1]; acc_n++; }
    else if (trim < 0 && acc_n > 1) acc_n--;
    const int16_t *p = acc;
    unsigned left = acc_n;
    while (left) {
        long n = p_writei(pcm, p, left);
        if (n < 0) {
            errors++;
            if (p_recover(pcm, (int)n, 1) < 0) { host_log("audio: %s", p_strerror((int)n)); break; }
            prefill();
            continue;
        }
        p += n * 2; left -= (unsigned)n;
    }
    acc_n = 0;
}

unsigned audio_errors(void) { return errors; }
long     audio_level_ms(void) { return level_frames < 0 ? -1 : level_frames * 1000 / 44100; }
bool     audio_active(void) { return pcm != NULL; }

void audio_close(void)
{
    if (pcm) p_close(pcm);
    pcm = NULL;
}
