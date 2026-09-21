/* bennugd: tiny libretro host for the MiSTer HPS.
 *
 * Runs the BennuGD_libretro core statically linked in, one retro_run() per
 * frame at the core's reported rate. Video goes to the DDR3 framebuffer the
 * FPGA scans (video.c), audio to ALSA (audio.c), input from evdev
 * (input.c). Started either directly with a .dat/.dcb path (ssh testing) or
 * as "bennugd --core" by bennugd-launcherd once Main_MiSTer has loaded the
 * BennuGD core; then everything comes from bennugd.cfg and Main keeps
 * running beside us (it owns the OSD, the video mode and the command FIFO).
 */
#define _GNU_SOURCE
#include "host.h"
#include "libretro.h"
#include "SDL.h"          /* the core's own SDL shim, statically linked: used only to log the surface format */
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CFG_PATH  "/media/fat/bennugd/bennugd.cfg"
#define FB_PHYS   0x22000000UL     /* must match FB_BASE in BennuGD.sv */
#define FB_W      416           /* SorR 5.2 renders 416x240 (widescreen); 4:3 games are centred */
#define FB_H      240
#define MAX_VARS  32
#define STAT_N    600

static struct {
    char *game, *log, *audio, *dump_dir, *prof;
    unsigned long fb;
    int dump_every, frames, menu, fps;
    int preread;                    /* MB/s for the page-cache pre-read; -1 auto, 0 off */
    int cpu;                    /* cpu= MHz, 0 leaves the clock alone */
    struct { char *key, *val; } vars[MAX_VARS];
    int nvars;
} opt = { .audio = "default", .preread = -1 };

static FILE *logf;
static struct retro_system_av_info av;
static char game_dir[1024];
static volatile sig_atomic_t stop;

void host_log(const char *fmt, ...)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    FILE *f = logf ? logf : stderr;
    fprintf(f, "[%6ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fflush(f);
}

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
    if (level == RETRO_LOG_DEBUG) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
    host_log("core: %s", buf);
}

static void set_var(const char *key, const char *val)
{
    for (int i = 0; i < opt.nvars; i++)
        if (!strcmp(opt.vars[i].key, key)) { opt.vars[i].val = strdup(val); return; }
    if (opt.nvars < MAX_VARS) {
        opt.vars[opt.nvars].key = strdup(key);
        opt.vars[opt.nvars].val = strdup(val);
        opt.nvars++;
    }
}

static const char *get_var(const char *key)
{
    for (int i = 0; i < opt.nvars; i++)
        if (!strcmp(opt.vars[i].key, key)) return opt.vars[i].val;
    return NULL;
}

/* one key=value setting, shared by the command line and bennugd.cfg */
static int apply_setting(const char *key, const char *val)
{
    if (!strcmp(key, "game")) opt.game = strdup(val);
    else if (!strcmp(key, "log")) opt.log = strdup(val);
    else if (!strcmp(key, "audio")) opt.audio = strdup(val);
    else if (!strcmp(key, "fb")) opt.fb = strtoul(val, NULL, 0);
    else if (!strcmp(key, "dump_dir")) opt.dump_dir = strdup(val);
    else if (!strcmp(key, "dump_every")) opt.dump_every = atoi(val);
    else if (!strcmp(key, "frames")) opt.frames = atoi(val);
    else if (!strcmp(key, "menu")) opt.menu = atoi(val);
    else if (!strcmp(key, "fps")) opt.fps = atoi(val);
    else if (!strcmp(key, "preread")) opt.preread = atoi(val);
    else if (!strcmp(key, "prof")) opt.prof = strdup(val);
    else if (!strcmp(key, "cpu")) opt.cpu = atoi(val);
    else if (!strncmp(key, "opt.", 4)) set_var(key + 4, val);
    else return -1;
    return 0;
}

static void read_cfg(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { host_log("cfg: cannot open %s", path); return; }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *p = line + strspn(line, " \t");
        if (*p == '#' || *p == '\n' || !*p) continue;
        p[strcspn(p, "\r\n")] = 0;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        if (apply_setting(p, eq + 1) < 0) host_log("cfg: unknown key %s", p);
    }
    fclose(f);
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format f = *(enum retro_pixel_format *)data;
        if (f != RETRO_PIXEL_FORMAT_RGB565) { host_log("env: pixel format %d refused", f); return false; }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = core_log; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        *(const char **)data = game_dir; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        v->value = get_var(v->key);
        return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        for (const struct retro_variable *v = data; v->key; v++)
            host_log("env: core option %s = %s", v->key, v->value);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool *)data = false; return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *(unsigned *)data = 0; return true;
    case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE: *(float *)data = 60.0f; return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        av = *(struct retro_system_av_info *)data;
        host_log("env: av info %ux%u @ %.3f fps, %.0f Hz",
                 av.geometry.base_width, av.geometry.base_height, av.timing.fps, av.timing.sample_rate);
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        av.geometry = *(struct retro_game_geometry *)data;
        host_log("env: geometry %ux%u", av.geometry.base_width, av.geometry.base_height);
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:      /* the game asked to quit */
        stop = 1; return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    video_present(data, w, h, pitch);
}
static void audio_cb(int16_t l, int16_t r)
{
    int16_t lr[2] = { l, r };
    audio_push(lr, 1);
}
static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    audio_push(data, frames);
    return frames;
}
static void on_signal(int sig) { (void)sig; stop = 2; }   /* 2: killed from outside, do not ask for the menu */

/* A crash leaves a black screen and, without this, nothing to go on: the
 * kernel logs the data address but not where the code was. Log both, then
 * die the normal way. */
static void on_crash(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    host_log("CRASH: signal %d at address %p, pc 0x%08lx lr 0x%08lx (symbolise pc with tools/profsym.py or nm on hps/out/bennugd)",
             sig, si->si_addr, (unsigned long)uc->uc_mcontext.arm_pc, (unsigned long)uc->uc_mcontext.arm_lr);
    signal(sig, SIG_DFL);
    raise(sig);
}

static long rss_kb(void)
{
    long pages = 0; FILE *f = fopen("/proc/self/statm", "r");
    if (f) { if (fscanf(f, "%*ld %ld", &pages) != 1) pages = 0; fclose(f); }
    return pages * (sysconf(_SC_PAGESIZE) / 1024);
}

/* Pull the game file into the page cache from a low-priority child, so
 * the sound effects BennuGD games load mid-level come from RAM instead of
 * the SD card (each cold read was a visible hitch). The kernel drops the
 * pages again if memory gets tight. */
/* Read the whole game file once into the page cache from a background
 * child, so the game's own reads of sounds and levels during play never
 * wait for the SD card or the network.
 * Rate: an unthrottled read starved the game of memory bandwidth for its
 * first minute (16 ms frames in the intro, audio queue near empty), so the
 * default is 4 MB/s from local storage. On a network filesystem every read
 * the game makes before the pre-read gets there costs a round trip and
 * shows as a dropped frame, so there the default is 16 MB/s: the file is
 * cached in 20 s instead of 80. preread=<MB/s> in bennugd.cfg overrides,
 * 0 disables. */
static void prewarm_file(const char *path, int mbps)
{
    if (mbps == 0) return;
    if (mbps > 1024) mbps = 1024;   /* above the storage's speed it is unthrottled anyway; keeps the arithmetic in range */
    if (mbps < 0) {
        /* filesystem type of the longest mount point containing the path */
        char fstype[32] = "?";
        size_t best = 0;
        FILE *m = fopen("/proc/mounts", "r");
        if (m) {
            char dev[256], mnt[256], type[32];
            while (fscanf(m, "%255s %255s %31s %*[^\n]", dev, mnt, type) == 3) {
                size_t n = strlen(mnt);
                if (n >= best && strncmp(path, mnt, n) == 0 && (n == 1 || path[n] == '/' || path[n] == 0)) {
                    best = n;
                    strncpy(fstype, type, sizeof fstype - 1);
                }
            }
            fclose(m);
        }
        int net = !strcmp(fstype, "cifs") || !strcmp(fstype, "smb3") || !strncmp(fstype, "nfs", 3) ||
                  !strncmp(fstype, "fuse", 4);
        mbps = net ? 16 : 4;
        host_log("pre-read: %s storage (%s), %d MB/s", net ? "network" : "local", fstype, mbps);
    }
    signal(SIGCHLD, SIG_IGN);      /* the child is reaped by the kernel, no zombie */
    if (fork() != 0) return;
    signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL);   /* the parent's handlers would make the child ignore a kill */
    setpriority(PRIO_PROCESS, 0, 19);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        static char buf[256 << 10];
        long ns = (long)(sizeof buf) * 1000000000L / ((long)mbps << 20);   /* per 256 KB chunk */
        struct timespec nap = { ns / 1000000000L, ns % 1000000000L };
        while (read(fd, buf, sizeof buf) > 0) nanosleep(&nap, NULL);
        close(fd);
    }
    _exit(0);
}

static int cmp_long(const void *a, const void *b)
{
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: bennugd <game.dat|--core> [key=value ...]\n"
        "  game= log= audio=(default|none|<alsa device>) fb=0x22000000|0 dump_dir= dump_every=N\n"
        "  frames=N menu=0|1 prof=<dump file> fps=0|1 preread=<MB/s|0> cpu=800|1000|1200 opt.<core option>=<value>\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(); return 2; }
    bool core_mode = !strcmp(argv[1], "--core");
    if (core_mode) { opt.fb = FB_PHYS; opt.menu = 1; opt.log = strdup("/media/fat/bennugd/bennugd.log"); read_cfg(CFG_PATH); }
    else opt.game = strdup(argv[1]);
    for (int i = 2; i < argc; i++) {
        char *arg = strdup(argv[i]);       /* a copy: cutting argv itself would change what ps shows */
        char *eq = strchr(arg, '=');
        if (!eq) { usage(); return 2; }
        *eq = 0;
        if (apply_setting(arg, eq + 1) < 0) { fprintf(stderr, "unknown setting %s\n", arg); return 2; }
        free(arg);
    }
    if (opt.log) { logf = fopen(opt.log, "a"); if (!logf) fprintf(stderr, "cannot open log %s\n", opt.log); }
    if (!opt.game) { host_log("no game= configured"); if (opt.menu) launcher_return_to_menu(); return 2; }

    char *tmp = strdup(opt.game);
    snprintf(game_dir, sizeof game_dir, "%s", dirname(tmp));
    free(tmp);
    if (chdir(game_dir) < 0) host_log("chdir %s failed", game_dir);
    host_log("bennugd: game %s (dir %s)", opt.game, game_dir);

    if (opt.cpu) launcher_set_cpu_mhz(opt.cpu);
    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    {
        struct sigaction sa = { .sa_sigaction = on_crash, .sa_flags = SA_SIGINFO };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    }

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll);
    retro_set_input_state(input_state);
    retro_init();

    struct retro_system_info si; retro_get_system_info(&si);
    host_log("core: %s %s (need_fullpath=%d)", si.library_name, si.library_version, si.need_fullpath);
    struct retro_game_info gi = { .path = opt.game };
    int gfd = -1;
    if (!si.need_fullpath) {
        struct stat st;
        gfd = open(opt.game, O_RDONLY);
        if (gfd < 0 || fstat(gfd, &st) < 0) { host_log("cannot open %s", opt.game); return 1; }
        gi.size = (size_t)st.st_size;
        gi.data = mmap(NULL, gi.size, PROT_READ, MAP_PRIVATE, gfd, 0);
        if (gi.data == MAP_FAILED) { host_log("mmap %s failed", opt.game); return 1; }
    }
    if (!retro_load_game(&gi)) { host_log("retro_load_game failed"); return 1; }
    {
        SDL_Surface *vs = SDL_GetVideoSurface();
        const SDL_VideoInfo *vi = SDL_GetVideoInfo();
        extern SDL_Surface *screen, *scale_screen; extern int scale_resolution;   /* BennuGD libvideo globals */
        if (scale_resolution != -1 && screen && scale_screen)
            host_log("sdl: BennuGD draws %dx%d and scales to %dx%d every frame (scale_resolution=%d)",
                     screen->w, screen->h, scale_screen->w, scale_screen->h, scale_resolution);
        if (vs && vi) host_log("sdl: surface %dx%d %d bpp masks %04x/%04x/%04x, native %d bpp%s",
                               vs->w, vs->h, vs->format->BitsPerPixel, vs->format->Rmask, vs->format->Gmask,
                               vs->format->Bmask, vi->vfmt->BitsPerPixel,
                               vs->format->BitsPerPixel != vi->vfmt->BitsPerPixel ? " (SHADOW SURFACE: extra convert per flip)" : "");
    }
    retro_get_system_av_info(&av);
    host_log("av: %ux%u (max %ux%u) @ %.3f fps, %.0f Hz", av.geometry.base_width, av.geometry.base_height,
             av.geometry.max_width, av.geometry.max_height, av.timing.fps, av.timing.sample_rate);

    if (video_init(opt.fb, FB_W, FB_H, opt.dump_dir, opt.dump_every) < 0 && opt.fb) return 1;
    /* with vsync the game runs at the FPGA's vblank rate, so play the audio
     * at that rate too (735 frames per 59.84 Hz frame = 43982 Hz, 0.27 % low,
     * inaudible) or the audio clock and the vblank clock drift apart */
    unsigned arate = (unsigned)av.timing.sample_rate;
    if (video_has_vsync() && av.timing.fps > 1.0)
        arate = (unsigned)(av.timing.sample_rate * video_vblank_hz() / av.timing.fps + 0.5);
    audio_init(opt.audio, arate);
    input_init(!core_mode);   /* beside Main_MiSTer, leave the devices shared so its OSD keeps working */

    prewarm_file(opt.game, opt.preread);
    if (opt.prof) prof_start(opt.prof);
    video_overlay(opt.fps);
    long times[STAT_N], runs[STAT_N]; int nt = 0; long worst = 0;
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    long frame = 0;
    while (!stop && !input_quit_requested() && (opt.frames == 0 || frame < opt.frames)) {
        struct timespec t0, t1;
        struct timespec tr;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        retro_run();
        clock_gettime(CLOCK_MONOTONIC, &tr);
        audio_flush();                 /* blocks on the ALSA buffer: this is the audio clock */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long run_us = (tr.tv_sec - t0.tv_sec) * 1000000L + (tr.tv_nsec - t0.tv_nsec) / 1000;
        video_overlay_run_us(run_us);
        video_wait_us_take();   /* the copier's vblank wait; not on this thread any more, keep the counter drained */
        long us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000;
        times[nt] = us; runs[nt] = run_us; nt++; if (run_us > worst) worst = run_us;
        if (opt.prof) { prof_frame_end(run_us > 16700); if (frame % 3600 == 0) prof_dump(); }
        frame++;
        if (nt == STAT_N) {
            qsort(times, STAT_N, sizeof *times, cmp_long);
            qsort(runs, STAT_N, sizeof *runs, cmp_long);
            host_log("stats: frame %ld run p50 %ld p99 %ld max %ld us | frame+audio p50 %ld p99 %ld us | fbcopy %ld us dropped %u | rss %ld kB audio_err %u level %ld ms",
                     frame, runs[STAT_N / 2], runs[STAT_N * 99 / 100], worst,
                     times[STAT_N / 2], times[STAT_N * 99 / 100], video_copy_us(), video_dropped(), rss_kb(), audio_errors(), audio_level_ms());
            nt = 0; worst = 0;
        }
        if (audio_active()) continue;   /* the full ALSA buffer is the clock; video never blocks */
        double fps = av.timing.fps > 1.0 ? av.timing.fps : 60.0;
        long period_ns = (long)(1e9 / fps);
        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (t1.tv_sec > next.tv_sec || (t1.tv_sec == next.tv_sec && t1.tv_nsec > next.tv_nsec))
            next = t1;   /* running behind: do not try to catch up */
        else
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    host_log("bennugd: stopping after %ld frames", frame);
    if (opt.prof) prof_dump();
    retro_unload_game();
    retro_deinit();
    sync();                         /* save files out of the page cache before anyone pulls the plug */
    input_close(); audio_close(); video_close();
    launcher_restore_cpu();
    if (gfd >= 0) close(gfd);
    if (opt.menu && stop != 2) launcher_return_to_menu();
    return 0;
}
