/* Audio through ALSA's "default" device. On MiSTer that chain is
 * plug -> rate 48 kHz -> file tee into /dev/MrAudio (the FPGA's alsa.sv
 * ring) -> hw card 0, a timer-driven dummy card. The dummy card gives the
 * blocking flow control that /dev/MrAudio itself lacks (its writes never
 * block, so late audio just becomes a gap). libasound is loaded at run
 * time so the cross build needs no ALSA headers or import library. */
#define _GNU_SOURCE
#include "host.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ACC_MAX     16384u    /* input frames buffered between flushes */
#define QUEUE_MS    90u       /* queued audio, default (audio_ms= in bennugd.cfg): it is the lag
                               * from an action to its sound, and the cushion over slow frames */
#define HEADROOM    1024u     /* ALSA buffer beyond the queue level: room for one top-up chunk */
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
static unsigned  topups;              /* extra chunks mixed because the game fell behind real time */
static int       vol_shift, vol_boost; /* OSD "Core Volume": attenuation in 6 dB steps, boost in 6 dB steps */
static time_t    vol_checked;

/* Main applies the OSD's Core Volume to the FPGA core's audio only (the
 * framework's filter stage sits before the Linux audio is mixed in), so it
 * never reaches sound that arrives through ALSA. Main also saves the value
 * to config/<core>_volume.cfg, one byte: bits 0-2 attenuate (shift right),
 * bits 5-6 boost. Apply that here, re-read once a second. */
static void read_core_volume(void)
{
    time_t now = time(NULL);
    if (now == vol_checked) return;
    vol_checked = now;
    int shift = 0, boost = 0;
    FILE *f = fopen("/media/fat/config/BennuGD_volume.cfg", "rb");
    if (f) {
        int c = fgetc(f); fclose(f);
        if (c != EOF) { shift = c & 7; if (shift > 6) shift = 6; boost = (c >> 5) & 3; if (boost > 2) boost = 2; }
    }
    if (shift != vol_shift || boost != vol_boost) {
        host_log("audio: core volume -%d dB%s", shift * 6, boost ? (boost == 1 ? " +6 dB boost" : " +12 dB boost") : "");
        vol_shift = shift; vol_boost = boost;
    }
}

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

int audio_init(const char *dev, unsigned rate, unsigned queue_ms)
{
    if (!queue_ms) queue_ms = QUEUE_MS;
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
    unsigned long buf = (unsigned long)rate * queue_ms / 1000 + HEADROOM, per = (unsigned long)rate * PERIOD_MS / 1000;
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
        if (++attempts < 5) { sleep(1); r = rate; buf = (unsigned long)rate * queue_ms / 1000 + HEADROOM; per = (unsigned long)rate * PERIOD_MS / 1000; goto retry; }
        p_hw_free(hw); p_close(pcm); pcm = NULL; return -1;
    }
    p_hw_free(hw);
    /* The level the queue is held at. The blocking writes in audio_flush cap
     * it at buf - 735 (one frame above it), regulate() trims it back down. */
    prefill_frames = (unsigned)(buf - HEADROOM);
    prefill();
    host_log("audio: %s, %u Hz S16 stereo, buffer %lu frames (%lu ms), period %lu frames, queue %u frames (%u ms)",
             dev, r, buf, buf * 1000 / r, per, prefill_frames, prefill_frames * 1000 / r);
    return 0;
}

void audio_push(const int16_t *lr, size_t frames)
{
    if (!pcm) return;
    if (acc_n + frames > ACC_MAX) { errors++; frames = ACC_MAX - acc_n; }
    read_core_volume();
    if (!vol_shift && !vol_boost) memcpy(acc + acc_n * 2, lr, frames * 4);
    else {
        int16_t *d = acc + acc_n * 2;
        for (size_t i = 0; i < frames * 2; i++) {
            int v = ((int)lr[i] << vol_boost) >> vol_shift;
            d[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
    }
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

/* The core mixes one frame's worth of audio (735 frames at 60 fps) per
 * retro_run, however long that run took. In a heavy scene at 17-18 ms a
 * frame the queue drains by a millisecond or two per frame, underruns
 * every few seconds and restarts with 90 ms of silence: the "crackle on
 * busy levels". When the queue has fallen a frame below the primed level,
 * have the core mix extra chunks (its RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK
 * entry, 1024 frames each, delivered through audio_push) until it is back
 * near the target. The music then plays on at the right speed and only
 * the picture slows down. A fast game never gets here: the blocking
 * writes in audio_flush keep the queue at the target. */
void audio_topup(void (*mix)(void))
{
    if (!pcm || !mix) return;
    for (int i = 0; i < 8; i++) {
        long d;
        if (p_delay(pcm, &d) < 0) return;
        /* a frame below the level: a slow frame drained it, put a chunk back
         * (the chunk fits: HEADROOM above the level is reserved for it) */
        if (d >= (long)prefill_frames - 735) return;
        mix();
        if (!acc_n) return;            /* the core had nothing for us */
        topups++;
        audio_flush();
    }
}

unsigned audio_errors(void) { return errors; }
unsigned audio_topups(void) { return topups; }
long     audio_level_ms(void) { return level_frames < 0 ? -1 : level_frames * 1000 / 44100; }
bool     audio_active(void) { return pcm != NULL; }

void audio_close(void)
{
    if (pcm) p_close(pcm);
    pcm = NULL;
}
