#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Main_MiSTer keeps running while we do (bennugd-launcherd starts us when
 * it sees our core in /tmp/CORENAME), so asking it for the menu is one
 * line into its command FIFO. Non-blocking open: if nobody is reading the
 * FIFO we must not hang here. */
/* CPU clock through the cpufreq driver of MiSTer Linux 20260912 and later
 * (drivers/cpufreq/socfpga-cpufreq.c: 400/800 MHz, 1000/1200 MHz behind
 * the boost switch, performance governor by default). Earlier images have
 * no cpufreq directory and stay at the stock 800 MHz. The clock is put
 * back to 800 MHz when the frontend exits: the overclock is the game's,
 * not the menu's. */
#define CPUFREQ "/sys/devices/system/cpu/cpu0/cpufreq/"
static bool cpu_changed;

static int sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}

static long sysfs_read_long(const char *path)
{
    FILE *f = fopen(path, "r"); long v = -1;
    if (f) { if (fscanf(f, "%ld", &v) != 1) v = -1; fclose(f); }
    return v;
}

void launcher_set_cpu_mhz(int mhz)
{
    if (access(CPUFREQ "scaling_max_freq", W_OK) < 0) {
        host_log("cpu: no cpufreq in this kernel (MiSTer Linux 20260912 or later has it), staying at stock 800 MHz");
        return;
    }
    char v[24]; snprintf(v, sizeof v, "%d000", mhz);
    if (mhz > 800) sysfs_write("/sys/devices/system/cpu/cpufreq/boost", "1");
    sysfs_write(CPUFREQ "scaling_governor", "performance");
    /* the kernel refuses max < min and min > max: lower min first, raise max first */
    if (mhz < 800) sysfs_write(CPUFREQ "scaling_min_freq", v);
    if (sysfs_write(CPUFREQ "scaling_max_freq", v) < 0) { host_log("cpu: cannot set %d MHz (%s)", mhz, strerror(errno)); return; }
    if (mhz >= 800) sysfs_write(CPUFREQ "scaling_min_freq", v);
    cpu_changed = mhz != 800;
    long cur = sysfs_read_long(CPUFREQ "scaling_cur_freq");
    host_log("cpu: asked for %d MHz, running at %ld MHz", mhz, cur / 1000);
}

void launcher_restore_cpu(void)
{
    if (!cpu_changed) return;
    cpu_changed = false;
    sysfs_write(CPUFREQ "scaling_min_freq", "800000");
    sysfs_write(CPUFREQ "scaling_max_freq", "800000");
    host_log("cpu: back to 800 MHz");
}

void launcher_return_to_menu(void)
{
    host_log("launcher: asking Main_MiSTer for the menu");
    int fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { host_log("launcher: /dev/MiSTer_cmd not readable by anyone (%s)", strerror(errno)); return; }
    const char cmd[] = "load_core /media/fat/menu.rbf\n";
    if (write(fd, cmd, sizeof cmd - 1) < 0) host_log("launcher: write failed (%s)", strerror(errno));
    close(fd);
}
