#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Main_MiSTer keeps running while we do (bennugd-launcherd starts us when
 * it sees our core in /tmp/CORENAME), so asking it for the menu is one
 * line into its command FIFO. Non-blocking open: if nobody is reading the
 * FIFO we must not hang here. */
void launcher_return_to_menu(void)
{
    host_log("launcher: asking Main_MiSTer for the menu");
    int fd = open("/dev/MiSTer_cmd", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { host_log("launcher: /dev/MiSTer_cmd not readable by anyone (%s)", strerror(errno)); return; }
    const char cmd[] = "load_core /media/fat/menu.rbf\n";
    if (write(fd, cmd, sizeof cmd - 1) < 0) host_log("launcher: write failed (%s)", strerror(errno));
    close(fd);
}
