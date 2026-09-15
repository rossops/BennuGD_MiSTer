#define _GNU_SOURCE
#include "host.h"
#include "libretro.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_DEVS 16
#define PORTS    2

struct dev {
    int  fd;
    int  port;          /* libretro port this device feeds */
    bool pad;           /* gamepad (else keyboard) */
    bool joystick;      /* old joystick-class pad: BTN_TRIGGER.. codes, no BTN_SOUTH */
    int  axis_min[2], axis_max[2];
};

static struct dev devs[MAX_DEVS];
static int      ndevs;
static uint16_t pad_bits[PORTS];       /* 1 << RETRO_DEVICE_ID_JOYPAD_x */
static uint16_t stick_bits[PORTS];     /* left stick as d-pad */
static uint8_t  keys[KEY_MAX + 1];
static bool     quit;
static uint16_t fpga_pad(unsigned port);
static int      log_presses = 40;   /* first presses go to the log: mapping questions answer themselves */

#define BIT(id) (1u << (id))

/* keyboard -> player 1 joypad */
static const struct { int key; int id; } kbd_map[] = {
    { KEY_UP,    RETRO_DEVICE_ID_JOYPAD_UP },    { KEY_DOWN,  RETRO_DEVICE_ID_JOYPAD_DOWN },
    { KEY_LEFT,  RETRO_DEVICE_ID_JOYPAD_LEFT },  { KEY_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT },
    { KEY_Z, RETRO_DEVICE_ID_JOYPAD_B }, { KEY_X, RETRO_DEVICE_ID_JOYPAD_A },
    { KEY_C, RETRO_DEVICE_ID_JOYPAD_Y }, { KEY_V, RETRO_DEVICE_ID_JOYPAD_X },
    { KEY_A, RETRO_DEVICE_ID_JOYPAD_L }, { KEY_S, RETRO_DEVICE_ID_JOYPAD_R },
    { KEY_ENTER, RETRO_DEVICE_ID_JOYPAD_START }, { KEY_SPACE, RETRO_DEVICE_ID_JOYPAD_SELECT },
};

static const struct { int btn; int id; } btn_map[] = {
    { BTN_SOUTH, RETRO_DEVICE_ID_JOYPAD_B }, { BTN_EAST,  RETRO_DEVICE_ID_JOYPAD_A },
    { BTN_WEST,  RETRO_DEVICE_ID_JOYPAD_Y }, { BTN_NORTH, RETRO_DEVICE_ID_JOYPAD_X },
    { BTN_TL, RETRO_DEVICE_ID_JOYPAD_L },    { BTN_TR, RETRO_DEVICE_ID_JOYPAD_R },
    { BTN_TL2, RETRO_DEVICE_ID_JOYPAD_L2 },  { BTN_TR2, RETRO_DEVICE_ID_JOYPAD_R2 },
    { BTN_SELECT, RETRO_DEVICE_ID_JOYPAD_SELECT }, { BTN_START, RETRO_DEVICE_ID_JOYPAD_START },
    { BTN_THUMBL, RETRO_DEVICE_ID_JOYPAD_L3 }, { BTN_THUMBR, RETRO_DEVICE_ID_JOYPAD_R3 },
    { BTN_DPAD_UP, RETRO_DEVICE_ID_JOYPAD_UP }, { BTN_DPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_DOWN },
    { BTN_DPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_LEFT }, { BTN_DPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_RIGHT },
};

/* joystick-class pads (e.g. Logitech Dual Action): buttons 1..12 report
 * BTN_TRIGGER..BTN_BASE6 (0x120..0x12b). The core hands libretro ids to
 * the game as SDL joystick buttons in a fixed order (A=0 B=1 X=2 Y=3 L=4
 * R=5 Select=6 Start=7 L3=8 R3=9 L2=10 R2=11), and BennuGD games such as
 * SorR store their control setup against those raw numbers. So map
 * physical button k to the id the core exposes as button k: the game then
 * sees exactly the numbering it saw under SDL on a PC. */
static const struct { int btn; int id; } js_map[] = {
    { BTN_TRIGGER, RETRO_DEVICE_ID_JOYPAD_A },  { BTN_THUMB,  RETRO_DEVICE_ID_JOYPAD_B },
    { BTN_THUMB2,  RETRO_DEVICE_ID_JOYPAD_X },  { BTN_TOP,    RETRO_DEVICE_ID_JOYPAD_Y },
    { BTN_TOP2, RETRO_DEVICE_ID_JOYPAD_L },     { BTN_PINKIE, RETRO_DEVICE_ID_JOYPAD_R },
    { BTN_BASE, RETRO_DEVICE_ID_JOYPAD_SELECT }, { BTN_BASE2, RETRO_DEVICE_ID_JOYPAD_START },
    { BTN_BASE3, RETRO_DEVICE_ID_JOYPAD_L3 },   { BTN_BASE4, RETRO_DEVICE_ID_JOYPAD_R3 },
    { BTN_BASE5, RETRO_DEVICE_ID_JOYPAD_L2 },   { BTN_BASE6, RETRO_DEVICE_ID_JOYPAD_R2 },
};

/* RETRO_DEVICE_KEYBOARD: libretro key -> evdev key, for what BennuGD games use */
static const struct { int rk; int ek; } key_map[] = {
    { RETROK_BACKSPACE, KEY_BACKSPACE }, { RETROK_TAB, KEY_TAB }, { RETROK_RETURN, KEY_ENTER },
    { RETROK_PAUSE, KEY_PAUSE }, { RETROK_ESCAPE, KEY_ESC }, { RETROK_SPACE, KEY_SPACE },
    { RETROK_0, KEY_0 }, { RETROK_1, KEY_1 }, { RETROK_2, KEY_2 }, { RETROK_3, KEY_3 },
    { RETROK_4, KEY_4 }, { RETROK_5, KEY_5 }, { RETROK_6, KEY_6 }, { RETROK_7, KEY_7 },
    { RETROK_8, KEY_8 }, { RETROK_9, KEY_9 },
    { RETROK_a, KEY_A }, { RETROK_b, KEY_B }, { RETROK_c, KEY_C }, { RETROK_d, KEY_D },
    { RETROK_e, KEY_E }, { RETROK_f, KEY_F }, { RETROK_g, KEY_G }, { RETROK_h, KEY_H },
    { RETROK_i, KEY_I }, { RETROK_j, KEY_J }, { RETROK_k, KEY_K }, { RETROK_l, KEY_L },
    { RETROK_m, KEY_M }, { RETROK_n, KEY_N }, { RETROK_o, KEY_O }, { RETROK_p, KEY_P },
    { RETROK_q, KEY_Q }, { RETROK_r, KEY_R }, { RETROK_s, KEY_S }, { RETROK_t, KEY_T },
    { RETROK_u, KEY_U }, { RETROK_v, KEY_V }, { RETROK_w, KEY_W }, { RETROK_x, KEY_X },
    { RETROK_y, KEY_Y }, { RETROK_z, KEY_Z },
    { RETROK_DELETE, KEY_DELETE }, { RETROK_UP, KEY_UP }, { RETROK_DOWN, KEY_DOWN },
    { RETROK_RIGHT, KEY_RIGHT }, { RETROK_LEFT, KEY_LEFT }, { RETROK_INSERT, KEY_INSERT },
    { RETROK_HOME, KEY_HOME }, { RETROK_END, KEY_END }, { RETROK_PAGEUP, KEY_PAGEUP },
    { RETROK_PAGEDOWN, KEY_PAGEDOWN },
    { RETROK_F1, KEY_F1 }, { RETROK_F2, KEY_F2 }, { RETROK_F3, KEY_F3 }, { RETROK_F4, KEY_F4 },
    { RETROK_F5, KEY_F5 }, { RETROK_F6, KEY_F6 }, { RETROK_F7, KEY_F7 }, { RETROK_F8, KEY_F8 },
    { RETROK_F9, KEY_F9 }, { RETROK_F10, KEY_F10 }, { RETROK_F11, KEY_F11 }, { RETROK_F12, KEY_F12 },
    { RETROK_RSHIFT, KEY_RIGHTSHIFT }, { RETROK_LSHIFT, KEY_LEFTSHIFT },
    { RETROK_RCTRL, KEY_RIGHTCTRL }, { RETROK_LCTRL, KEY_LEFTCTRL },
    { RETROK_RALT, KEY_RIGHTALT }, { RETROK_LALT, KEY_LEFTALT },
    { RETROK_KP0, KEY_KP0 }, { RETROK_KP1, KEY_KP1 }, { RETROK_KP2, KEY_KP2 },
    { RETROK_KP3, KEY_KP3 }, { RETROK_KP4, KEY_KP4 }, { RETROK_KP5, KEY_KP5 },
    { RETROK_KP6, KEY_KP6 }, { RETROK_KP7, KEY_KP7 }, { RETROK_KP8, KEY_KP8 },
    { RETROK_KP9, KEY_KP9 }, { RETROK_KP_ENTER, KEY_KPENTER },
    { RETROK_MINUS, KEY_MINUS }, { RETROK_EQUALS, KEY_EQUAL },
    { RETROK_LEFTBRACKET, KEY_LEFTBRACE }, { RETROK_RIGHTBRACKET, KEY_RIGHTBRACE },
    { RETROK_SEMICOLON, KEY_SEMICOLON }, { RETROK_QUOTE, KEY_APOSTROPHE },
    { RETROK_BACKQUOTE, KEY_GRAVE }, { RETROK_BACKSLASH, KEY_BACKSLASH },
    { RETROK_COMMA, KEY_COMMA }, { RETROK_PERIOD, KEY_DOT }, { RETROK_SLASH, KEY_SLASH },
};

static bool has_bit(const unsigned long *bits, int b)
{
    return (bits[b / (8 * sizeof(unsigned long))] >> (b % (8 * sizeof(unsigned long)))) & 1;
}

int input_init(bool grab)
{
    DIR *d = opendir("/dev/input");
    if (!d) { host_log("input: no /dev/input"); return -1; }
    struct dirent *e;
    int pads = 0;
    while ((e = readdir(d)) && ndevs < MAX_DEVS) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[64];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned long kb[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {0};
        unsigned long ab[(ABS_MAX + 1) / (8 * sizeof(unsigned long)) + 1] = {0};
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof ab), ab);
        bool gamepad = has_bit(kb, BTN_SOUTH) || has_bit(kb, BTN_START);
        bool joystick = !gamepad && has_bit(kb, BTN_TRIGGER) && has_bit(ab, ABS_X);
        bool pad = gamepad || joystick;
        bool kbd = has_bit(kb, KEY_ENTER) && has_bit(kb, KEY_Z);
        if (!pad && !kbd) { close(fd); continue; }
        char name[64] = "?";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        struct dev *dv = &devs[ndevs++];
        dv->fd = fd; dv->pad = pad; dv->joystick = joystick;
        dv->port = pad ? (pads < PORTS ? pads++ : PORTS - 1) : 0;
        for (int a = 0; a < 2; a++) {
            struct input_absinfo ai;
            if (has_bit(ab, ABS_X + a) && !ioctl(fd, EVIOCGABS(ABS_X + a), &ai)) {
                dv->axis_min[a] = ai.minimum; dv->axis_max[a] = ai.maximum;
            }
        }
        if (grab) ioctl(fd, EVIOCGRAB, 1);
        host_log("input: %s \"%s\" -> %s port %d", path, name,
                 joystick ? "joystick-class pad" : pad ? "pad" : "keyboard", dv->port);
    }
    closedir(d);
    return 0;
}

static void set_bit(uint16_t *w, int id, bool on)
{
    if (on) *w |= BIT(id); else *w &= (uint16_t)~BIT(id);
}

static void handle(struct dev *dv, const struct input_event *ev)
{
    if (ev->type == EV_KEY) {
        bool on = ev->value != 0;
        if (on && ev->value != 2 && log_presses > 0) { log_presses--; host_log("input: press 0x%03x on %s port %d", ev->code, dv->pad ? "pad" : "keyboard", dv->port); }
        if (ev->code <= KEY_MAX) keys[ev->code] = on;
        if (dv->pad && dv->joystick) {
            for (size_t i = 0; i < sizeof js_map / sizeof *js_map; i++)
                if (js_map[i].btn == ev->code) set_bit(&pad_bits[dv->port], js_map[i].id, on);
        } else if (dv->pad) {
            for (size_t i = 0; i < sizeof btn_map / sizeof *btn_map; i++)
                if (btn_map[i].btn == ev->code) set_bit(&pad_bits[dv->port], btn_map[i].id, on);
        } else {
            for (size_t i = 0; i < sizeof kbd_map / sizeof *kbd_map; i++)
                if (kbd_map[i].key == ev->code) set_bit(&pad_bits[0], kbd_map[i].id, on);
            if (ev->code == KEY_END && on) quit = true;
        }
    } else if (ev->type == EV_ABS && dv->pad) {
        uint16_t *s = &stick_bits[dv->port];
        if (ev->code == ABS_HAT0X) {
            set_bit(&pad_bits[dv->port], RETRO_DEVICE_ID_JOYPAD_LEFT,  ev->value < 0);
            set_bit(&pad_bits[dv->port], RETRO_DEVICE_ID_JOYPAD_RIGHT, ev->value > 0);
        } else if (ev->code == ABS_HAT0Y) {
            set_bit(&pad_bits[dv->port], RETRO_DEVICE_ID_JOYPAD_UP,   ev->value < 0);
            set_bit(&pad_bits[dv->port], RETRO_DEVICE_ID_JOYPAD_DOWN, ev->value > 0);
        } else if (ev->code == ABS_X || ev->code == ABS_Y) {
            int a = ev->code - ABS_X;
            int range = dv->axis_max[a] - dv->axis_min[a];
            if (range <= 0) return;
            int c = dv->axis_min[a] + range / 2, dz = range / 4;
            int neg = a ? RETRO_DEVICE_ID_JOYPAD_UP : RETRO_DEVICE_ID_JOYPAD_LEFT;
            int pos = a ? RETRO_DEVICE_ID_JOYPAD_DOWN : RETRO_DEVICE_ID_JOYPAD_RIGHT;
            set_bit(s, neg, ev->value < c - dz);
            set_bit(s, pos, ev->value > c + dz);
        }
    }
}

void input_poll(void)
{
    struct pollfd pf[MAX_DEVS];
    for (int i = 0; i < ndevs; i++) { pf[i].fd = devs[i].fd; pf[i].events = POLLIN; }
    if (poll(pf, ndevs, 0) <= 0) return;
    for (int i = 0; i < ndevs; i++) {
        if (!(pf[i].revents & POLLIN)) continue;
        struct input_event ev[32];
        ssize_t n;
        while ((n = read(devs[i].fd, ev, sizeof ev)) > 0)
            for (size_t k = 0; k < n / sizeof ev[0]; k++) handle(&devs[i], &ev[k]);
    }
    /* exit chord: the two small centre buttons together. Select+Start on a
     * gamepad; on a joystick-class pad those are buttons 9+10, which the
     * passthrough map above exposes as L3+R3. */
    for (int p = 0; p < PORTS; p++) {
        uint16_t b = pad_bits[p] | stick_bits[p] | fpga_pad(p);
        const uint16_t ss = BIT(RETRO_DEVICE_ID_JOYPAD_SELECT) | BIT(RETRO_DEVICE_ID_JOYPAD_START);
        const uint16_t js = BIT(RETRO_DEVICE_ID_JOYPAD_L3) | BIT(RETRO_DEVICE_ID_JOYPAD_R3);
        if ((b & ss) == ss || (b & js) == js) quit = true;
    }
}

/* hps_io joystick word (Main_MiSTer's OSD mapping, via the control block):
 * [0] right [1] left [2] down [3] up, then the CONF_STR J line:
 * A B X Y L R Select Start in bits 4..11 */
static uint16_t fpga_pad(unsigned port)
{
    const volatile uint32_t *j = video_joystick_words();
    if (!j) return 0;
    uint32_t w = j[port];
    uint16_t b = 0;
    if (w & 1) b |= BIT(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (w & 2) b |= BIT(RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (w & 4) b |= BIT(RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (w & 8) b |= BIT(RETRO_DEVICE_ID_JOYPAD_UP);
    static const int ids[8] = { RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_X,
        RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START };
    for (int i = 0; i < 8; i++) if (w & (16u << i)) b |= BIT(ids[i]);
    return b;
}

int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (device == RETRO_DEVICE_JOYPAD && port < PORTS) {
        uint16_t b = pad_bits[port] | stick_bits[port] | fpga_pad(port);
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)b;
        return id < 16 && (b & BIT(id)) ? 1 : 0;
    }
    if (device == RETRO_DEVICE_KEYBOARD && port == 0) {
        for (size_t i = 0; i < sizeof key_map / sizeof *key_map; i++)
            if ((unsigned)key_map[i].rk == id) return keys[key_map[i].ek];
    }
    return 0;
}

bool input_quit_requested(void) { return quit; }

void input_close(void)
{
    for (int i = 0; i < ndevs; i++) { ioctl(devs[i].fd, EVIOCGRAB, 0); close(devs[i].fd); }
    ndevs = 0;
}
