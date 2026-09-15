/* In-process PC sampler for finding where slow frames spend their time.
 * A 1 kHz SIGPROF timer records the interrupted program counter; after
 * each frame the caller keeps the frame's samples only if the frame ran
 * over budget. The kept (pc, lr) pairs are dumped as raw uint32 values and
 * symbolised on the Mac with tools/profsym.py against hps/out/bennugd.
 * Enabled with prof=<path>; costs nothing when off. */
#define _GNU_SOURCE
#include "host.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define FRAME_CAP 4096          /* samples per frame (4 s at 1 kHz, plenty) */
#define KEEP_CAP  (1u << 20)    /* 1M kept samples, 4 MB */

static uint32_t frame_pcs[FRAME_CAP * 2];   /* pc, lr pairs: lr attributes leaf functions such as memcpy to their caller */
static volatile unsigned frame_n;
static uint32_t *kept;
static unsigned  kept_n;
static const char *dump_path;
static unsigned  slow_frames, total_frames;

static void on_prof(int sig, siginfo_t *si, void *ctx)
{
    (void)sig; (void)si;
    ucontext_t *uc = ctx;
    unsigned n = frame_n;
    if (n < FRAME_CAP) {
        frame_pcs[n * 2]     = (uint32_t)uc->uc_mcontext.arm_pc;
        frame_pcs[n * 2 + 1] = (uint32_t)uc->uc_mcontext.arm_lr;
        frame_n = n + 1;
    }
}

int prof_start(const char *path)
{
    dump_path = path;
    kept = malloc(KEEP_CAP * 2 * sizeof *kept);
    if (!kept) return -1;
    struct sigaction sa = { .sa_sigaction = on_prof, .sa_flags = SA_SIGINFO | SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, NULL);
    host_log("prof: sampling at 1 kHz, keeping frames over budget, dump to %s", path);
    return 0;
}

void prof_frame_end(bool keep)
{
    total_frames++;
    if (keep) {
        slow_frames++;
        unsigned n = frame_n;
        if (kept_n + n > KEEP_CAP) n = KEEP_CAP - kept_n;
        memcpy(kept + kept_n * 2, frame_pcs, n * 2 * sizeof *kept);
        kept_n += n;
    }
    frame_n = 0;
}

void prof_dump(void)
{
    if (!dump_path || !kept) return;
    FILE *f = fopen(dump_path, "wb");
    if (!f) { host_log("prof: cannot write %s", dump_path); return; }
    fwrite(kept, 2 * sizeof *kept, kept_n, f);
    fclose(f);
    host_log("prof: %u samples from %u slow frames of %u written to %s", kept_n, slow_frames, total_frames, dump_path);
}
