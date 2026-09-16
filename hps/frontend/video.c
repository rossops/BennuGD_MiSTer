/* Video: the FPGA scans an RGB565 buffer in DDR3. Two ways to drive it:
 *
 *  - with the control block (rtl/bennugd_ctl.sv): two buffers, the HPS
 *    renders into the back one, publishes its index and geometry in the
 *    block, and waits for the FPGA's vblank counter to move. Page flip and
 *    vsync, no tearing.
 *  - without it (old cores, or a plain test from ssh): one fixed 416x240
 *    buffer, the surface centred into it, no sync.
 * The block is detected at start by watching the vblank counter.
 * PPM dumps of the core surface every N frames serve headless testing. */
#define _GNU_SOURCE
#include "host.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <arm_neon.h>

/* The framebuffer mapping is uncached device memory: plain memcpy stores
 * 4 bytes at a time and every store goes to DDR. 16-byte NEON stores cut
 * the number of bus transactions; rows are 2-byte pixels so handle the
 * unaligned tail with a plain copy. */
static void copy_row(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n >= 64) {
        uint8x16_t a = vld1q_u8(src), b = vld1q_u8(src + 16), c = vld1q_u8(src + 32), d = vld1q_u8(src + 48);
        vst1q_u8(dst, a); vst1q_u8(dst + 16, b); vst1q_u8(dst + 32, c); vst1q_u8(dst + 48, d);
        src += 64; dst += 64; n -= 64;
    }
    while (n >= 16) { vst1q_u8(dst, vld1q_u8(src)); src += 16; dst += 16; n -= 16; }
    if (n) memcpy(dst, src, n);
}

#define CTL_PHYS     0x23F00000UL
#define FB_STRIDE_BUF 0x100000UL      /* 1 MB per buffer */
#define CTL_PRESENT  0
#define CTL_WIDTH    1
#define CTL_HEIGHT   2
#define CTL_STRIDE   3
#define CTL_VBLANK   8
#define CTL_JOY0     9

static int       memfd = -1;
static uint8_t  *fb;          /* both buffers, 2 MB */
static size_t    fb_len;
static volatile uint32_t *ctl;
static bool      have_ctl;
static unsigned  fixed_w, fixed_h;
static unsigned  cur_w, cur_h;    /* geometry published in the block */
/* The uncached copy into DDR3 costs 2.5 ms whatever the instruction mix,
 * about 15 % of a frame. It runs on a helper thread (the second A9 core
 * is mostly idle) while the game computes the next frame: video_present
 * only copies the core surface into a cached staging buffer and hands it
 * over. The helper also does the flip bookkeeping and the vblank wait. */
static pthread_t       copier;
static void *copier_main(void *arg);
static void pin_to_cpu(int cpu);
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv  = PTHREAD_COND_INITIALIZER;
static uint8_t        *staging[2];       /* cached, w*h*2 each */
static unsigned        staging_w, staging_h;
static int             latest = -1;      /* staging index holding the newest complete frame, -1 none */
static int             reading = -1;     /* staging index the copier is copying right now */
static bool            copier_run;
static int       pending = -1;    /* buffer published, shown from the next vblank on */
static uint32_t  published_v;     /* vblank count when it was published */
static unsigned  dropped;
static unsigned  replaced;        /* frames replaced by a newer one before the copier took them */
static double    vblank_hz;
static const char *dumpdir;
static int       dump_every;
static unsigned  frames;
static long      copy_us_total;
static long      wait_us_total;   /* time spent waiting for the vblank, so main.c can take it out of 'run' */
static unsigned  copy_n;

static uint32_t wait_vblank_change(uint32_t seen, long timeout_us)
{
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint32_t v = ctl[CTL_VBLANK];
        if (v != seen) return v;
        clock_gettime(CLOCK_MONOTONIC, &t);
        if ((t.tv_sec - t0.tv_sec) * 1000000L + (t.tv_nsec - t0.tv_nsec) / 1000 > timeout_us) return seen;
        struct timespec nap = { 0, 500000 };   /* 0.5 ms: fine enough against a 16.7 ms frame */
        nanosleep(&nap, NULL);
    }
}

int video_init(unsigned long fb_phys, unsigned w, unsigned h, const char *dir, int every)
{
    dumpdir = dir;
    dump_every = every;
    if (!fb_phys) return 0;
    fixed_w = w; fixed_h = h;
    memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { host_log("video: open /dev/mem failed"); return -1; }
    fb_len = 2 * FB_STRIDE_BUF;
    fb = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, (off_t)fb_phys);
    if (fb == MAP_FAILED) { host_log("video: mmap 0x%lx failed", fb_phys); fb = NULL; return -1; }
    ctl = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, (off_t)CTL_PHYS);
    if (ctl == MAP_FAILED) { host_log("video: mmap control block failed"); ctl = NULL; }
    memset(fb, 0, fb_len);

    if (ctl) {
        /* does anyone update the vblank counter? then the core has the control block */
        ctl[CTL_PRESENT] = 0; ctl[CTL_WIDTH] = 0; ctl[CTL_HEIGHT] = 0; ctl[CTL_STRIDE] = 0;
        /* three tries: right after a core load the FPGA may still be in reset */
        for (int attempt = 0; attempt < 3 && !have_ctl; attempt++) {
            uint32_t v0 = ctl[CTL_VBLANK];
            uint32_t v1 = wait_vblank_change(v0, 300000);
            have_ctl = v1 != v0;
            host_log("video: control block probe %d: vblank counter %u -> %u%s", attempt + 1, v0, v1,
                     have_ctl ? "" : " (not moving)");
        }
    }
    if (have_ctl) {
        /* rough vblank rate, for the log and for the audio clock */
        uint32_t v = ctl[CTL_VBLANK];
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < 30; i++) v = wait_vblank_change(v, 50000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        vblank_hz = 30.0 / s;
        host_log("video: control block found, two buffers at 0x%lx, vblank %.2f Hz", fb_phys, vblank_hz);
        copier_run = true;
        pin_to_cpu(0);
        pthread_create(&copier, NULL, copier_main, NULL);
    } else {
        host_log("video: no control block, single %ux%u buffer at 0x%lx", w, h, fb_phys);
    }
    return 0;
}

bool   video_has_vsync(void) { return have_ctl; }
double video_vblank_hz(void) { return vblank_hz; }

const volatile uint32_t *video_joystick_words(void) { return have_ctl ? ctl + CTL_JOY0 : NULL; }

static void pin_to_cpu(int cpu)
{
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof set, &set);   /* the calling thread */
}

static void *copier_main(void *arg)
{
    (void)arg;
    /* Main_MiSTer busy-polls the FPGA on CPU 1 while a core is loaded (the
     * daemon lowers its priority). The game thread gets CPU 0 to itself;
     * the copier shares CPU 1 with Main's polling. */
    pin_to_cpu(1);
    for (;;) {
        /* 1. wait until the FPGA has latched the last published buffer, so
         *    the other one is free. Nothing is held during this wait: the
         *    game keeps running at the audio clock and may replace the
         *    frame waiting for us with a newer one. */
        uint32_t v = ctl[CTL_VBLANK];
        if (pending >= 0 && v == published_v) {
            struct timespec w0, w1; clock_gettime(CLOCK_MONOTONIC, &w0);
            v = wait_vblank_change(v, 20000);
            clock_gettime(CLOCK_MONOTONIC, &w1);
            wait_us_total += (w1.tv_sec - w0.tv_sec) * 1000000L + (w1.tv_nsec - w0.tv_nsec) / 1000;
        }
        /* 2. take the newest frame */
        pthread_mutex_lock(&mtx);
        while (copier_run && latest < 0) pthread_cond_wait(&cv, &mtx);
        if (!copier_run) { pthread_mutex_unlock(&mtx); break; }
        reading = latest; latest = -1;
        unsigned w = staging_w, h = staging_h;
        pthread_mutex_unlock(&mtx);

        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        if (pending >= 0 && v == published_v) {
            dropped++;                   /* the counter is not moving: no core? */
        } else {
            int target = pending < 0 ? 0 : pending ^ 1;
            /* rows start on 64-byte boundaries in DDR: the mapping is device
             * memory and the FPGA reads bursts; the stride is published */
            unsigned stride = (w * 2 + 63) & ~63u;
            if (w != cur_w || h != cur_h) {
                ctl[CTL_WIDTH] = w; ctl[CTL_HEIGHT] = h; ctl[CTL_STRIDE] = stride;
                cur_w = w; cur_h = h;
                host_log("video: geometry %ux%u stride %u", w, h, stride);
            }
            uint8_t *dst = fb + target * FB_STRIDE_BUF;
            for (unsigned y = 0; y < h; y++)
                copy_row(dst + y * stride, staging[reading] + y * w * 2, w * 2);
            ctl[CTL_PRESENT] = (uint32_t)target;
            pending = target;
            published_v = v;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        copy_us_total += (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000; copy_n++;

        pthread_mutex_lock(&mtx);
        reading = -1;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mtx);
    }
    return NULL;
}

static void dump_ppm(const uint16_t *src, unsigned w, unsigned h, size_t pitch)
{
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%06u.ppm", dumpdir, frames);
    FILE *f = fopen(path, "wb");
    if (!f) { host_log("video: cannot write %s", path); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)src + y * pitch);
        for (unsigned x = 0; x < w; x++) {
            uint16_t p = row[x];
            uint8_t rgb[3] = { (uint8_t)(((p >> 11) & 31) * 255 / 31),
                               (uint8_t)(((p >> 5) & 63) * 255 / 63),
                               (uint8_t)((p & 31) * 255 / 31) };
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

void video_present(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (!data) return;          /* duplicate frame */
    frames++;
    if (fb) {
        const uint8_t *src = data;
        if (have_ctl) {
            if (w > 2048) w = 2048;
            if (h * ((w * 2 + 63) & ~63u) > FB_STRIDE_BUF) h = FB_STRIDE_BUF / ((w * 2 + 63) & ~63u);
            pthread_mutex_lock(&mtx);
            if (!staging[0] || staging_w != w || staging_h != h) {
                while (reading >= 0) pthread_cond_wait(&cv, &mtx);    /* resize only when the copier is idle */
                free(staging[0]); free(staging[1]);
                staging[0] = malloc((size_t)w * h * 2); staging[1] = malloc((size_t)w * h * 2);
                staging_w = w; staging_h = h; latest = -1;
            }
            /* write the buffer the copier is not reading; if a frame is still
             * waiting there it is simply replaced (the game outran a vblank) */
            int idx = reading == 0 ? 1 : 0;
            if (latest >= 0 && latest != idx) { /* both hold data: overwrite the waiting one */ }
            else if (latest == idx) replaced++;
            pthread_mutex_unlock(&mtx);
            for (unsigned y = 0; y < h; y++)
                memcpy(staging[idx] + y * w * 2, src + y * pitch, w * 2);   /* cached: cheap */
            pthread_mutex_lock(&mtx);
            latest = idx;
            pthread_cond_broadcast(&cv);
            pthread_mutex_unlock(&mtx);
        } else {
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            unsigned cw = w < fixed_w ? w : fixed_w, ch = h < fixed_h ? h : fixed_h;
            unsigned ox = (fixed_w - cw) / 2, oy = (fixed_h - ch) / 2;
            uint16_t *dst = (uint16_t *)fb;
            for (unsigned y = 0; y < ch; y++)
                copy_row((uint8_t *)(dst + (oy + y) * fixed_w + ox), src + y * pitch, cw * 2);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            copy_us_total += (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000; copy_n++;
        }
    }
    if (dump_every > 0 && dumpdir && frames % dump_every == 0)
        dump_ppm(data, w, h, pitch);
}

long video_copy_us(void)
{
    long avg = copy_n ? copy_us_total / copy_n : 0;
    copy_us_total = 0; copy_n = 0;
    return avg;
}

long video_wait_us_take(void) { long w = wait_us_total; wait_us_total = 0; return w; }

unsigned video_dropped(void) { unsigned d = dropped + replaced; dropped = 0; replaced = 0; return d; }

void video_close(void)
{
    if (copier_run) {
        pthread_mutex_lock(&mtx); copier_run = false; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mtx);
        pthread_join(copier, NULL);
        free(staging[0]); free(staging[1]); staging[0] = staging[1] = NULL;
    }
    if (ctl) { ctl[CTL_PRESENT] = 0; munmap((void *)ctl, 4096); }
    if (fb) munmap(fb, fb_len);
    if (memfd >= 0) close(memfd);
    fb = NULL; ctl = NULL; memfd = -1; have_ctl = false;
}
