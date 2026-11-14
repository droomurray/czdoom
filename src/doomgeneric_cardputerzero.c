/*
 * doomgeneric platform backend for the M5Stack CardputerZero.
 *
 *   video : Linux fbdev (the 320x170 RGB565 ST7789 panel, /dev/fb_lcd)
 *   input : Linux evdev (the built-in TCA8418 keyboard, plus any USB keyboard)
 *   audio : SDL2_mixer, provided by doomgeneric's own i_sdlsound/i_sdlmusic
 *
 * Doom renders at 320x200. The panel is 320x170, so every frame is scaled
 * down vertically with a two-row blend, which keeps the status bar digits
 * and menu text readable where plain row-dropping would not.
 *
 * Copyright (C) 2026 czdoom contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>

/* linux/input.h and doomkeys.h both define KEY_* names, so the evdev codes
 * this file needs are spelled out here instead of including linux/input.h. */
struct evdev_event {
    struct timeval time;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#define EVDEV_EV_KEY 0x01
#define EVDEV_IOC_GBIT_KEY(len) _IOC(_IOC_READ, 'E', 0x20 + EVDEV_EV_KEY, len)

enum {
    EK_ESC = 1, EK_1 = 2, EK_0 = 11, EK_MINUS = 12, EK_EQUAL = 13,
    EK_BACKSPACE = 14, EK_TAB = 15, EK_Q = 16, EK_P = 25, EK_ENTER = 28,
    EK_LEFTCTRL = 29, EK_A = 30, EK_L = 38, EK_LEFTSHIFT = 42, EK_Z = 44,
    EK_M = 50, EK_COMMA = 51, EK_DOT = 52, EK_RIGHTSHIFT = 54,
    EK_LEFTALT = 56, EK_SPACE = 57, EK_F1 = 59, EK_F10 = 68, EK_F11 = 87,
    EK_F12 = 88, EK_KPENTER = 96, EK_RIGHTCTRL = 97, EK_RIGHTALT = 100,
    EK_UP = 103, EK_LEFT = 105, EK_RIGHT = 106, EK_DOWN = 108,
    EK_PAUSE = 119, EK_MAX = 0x2ff
};

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"

#define SRC_W DOOMGENERIC_RESX
#define SRC_H DOOMGENERIC_RESY

/* ------------------------------------------------------------------ video */

static struct {
    int fd;
    uint8_t *map;       /* mmap of the device, or NULL when using pwrite() */
    size_t map_len;
    uint8_t *stage;     /* one finished frame in device pixel format */
    size_t stage_len;
    int w, h, bytespp, stride;
    size_t origin;      /* byte offset of the visible area in the device */
    int r_off, r_len, g_off, g_len, b_off, b_len;
    int is_rgb565;
    int fake;           /* a plain file standing in for the device (tests) */
    /* where the picture goes inside the panel */
    int dx, dy, dw, dh;
} fb = { .fd = -1 };

/* For each destination column/row: the two source samples and the weight
 * (0..256) of the second one. */
static uint16_t *x0_tab, *x1_tab, *xw_tab;
static uint16_t *y0_tab, *y1_tab, *yw_tab;
static uint32_t row_tmp[SRC_W];

static void build_axis(int dst_n, int src_n, uint16_t **i0, uint16_t **i1, uint16_t **w)
{
    *i0 = malloc(sizeof(uint16_t) * dst_n);
    *i1 = malloc(sizeof(uint16_t) * dst_n);
    *w = malloc(sizeof(uint16_t) * dst_n);
    for (int i = 0; i < dst_n; i++) {
        /* sample at pixel centres, in 1/256 source pixels */
        long pos = ((2L * i + 1) * src_n * 256) / (2L * dst_n) - 128;
        if (pos < 0) pos = 0;
        long a = pos >> 8;
        if (a > src_n - 1) a = src_n - 1;
        long b = a + 1 > src_n - 1 ? src_n - 1 : a + 1;
        (*i0)[i] = (uint16_t)a;
        (*i1)[i] = (uint16_t)b;
        (*w)[i] = (dst_n == src_n) ? 0 : (uint16_t)(pos & 0xff);
    }
}

static inline uint32_t mix(uint32_t a, uint32_t b, uint32_t w)
{
    uint32_t rb = ((((a & 0xff00ff) * (256 - w)) + ((b & 0xff00ff) * w)) >> 8) & 0xff00ff;
    uint32_t g = ((((a & 0x00ff00) * (256 - w)) + ((b & 0x00ff00) * w)) >> 8) & 0x00ff00;
    return rb | g;
}

static inline uint32_t pack(uint32_t xrgb)
{
    uint32_t r = (xrgb >> 16) & 0xff, g = (xrgb >> 8) & 0xff, b = xrgb & 0xff;
    return ((r >> (8 - fb.r_len)) << fb.r_off) |
           ((g >> (8 - fb.g_len)) << fb.g_off) |
           ((b >> (8 - fb.b_len)) << fb.b_off);
}

static void fb_flush(void)
{
    if (fb.map) {
        memcpy(fb.map + fb.origin, fb.stage, fb.stage_len);
    } else if (pwrite(fb.fd, fb.stage, fb.stage_len, (off_t)fb.origin) < 0) {
        /* nothing useful to do about a failed frame */
    }
}

static void fb_blank(void)
{
    if (fb.fd < 0 || !fb.stage || fb.fake) return; /* tests keep the last frame */
    memset(fb.stage, 0, fb.stage_len);
    fb_flush();
}

static const char *find_fb_device(void)
{
    static char path[64];
    const char *env = getenv("CZDOOM_FB");
    if (env && *env) return env;

    /* udev rule shipped with the CardputerZero image */
    if (access("/dev/fb_lcd", F_OK) == 0) return "/dev/fb_lcd";

    FILE *fp = fopen("/proc/fb", "r");
    if (fp) {
        char line[256];
        int n = -1;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "st7789") || strstr(line, "mipi-dbi") || strstr(line, "mipi_dbi")) {
                if (sscanf(line, "%d", &n) == 1) break;
                n = -1;
            }
        }
        fclose(fp);
        if (n >= 0) {
            snprintf(path, sizeof(path), "/dev/fb%d", n);
            return path;
        }
    }
    return "/dev/fb0";
}

static void fb_open(void)
{
    const char *path = find_fb_device();
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int bpp;

    fb.fd = open(path, O_RDWR);
    if (fb.fd < 0) {
        fprintf(stderr, "czdoom: cannot open %s: %s\n", path, strerror(errno));
        fprintf(stderr, "czdoom: is this user in the 'video' group?\n");
        exit(1);
    }

    memset(&v, 0, sizeof(v));
    memset(&f, 0, sizeof(f));
    if (ioctl(fb.fd, FBIOGET_VSCREENINFO, &v) == 0 && ioctl(fb.fd, FBIOGET_FSCREENINFO, &f) == 0) {
        fb.w = (int)v.xres;
        fb.h = (int)v.yres;
        bpp = (int)v.bits_per_pixel;
        fb.stride = (int)f.line_length;
        fb.r_off = (int)v.red.offset;   fb.r_len = (int)v.red.length;
        fb.g_off = (int)v.green.offset; fb.g_len = (int)v.green.length;
        fb.b_off = (int)v.blue.offset;  fb.b_len = (int)v.blue.length;
        fb.origin = (size_t)v.yoffset * f.line_length + (size_t)v.xoffset * (bpp / 8);
        fb.map_len = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres_virtual;
    } else {
        /* Not a framebuffer device. CZDOOM_FAKEFB=WxHxBPP lets a plain file
         * stand in for one, which is how the port is tested off-device. */
        const char *fake = getenv("CZDOOM_FAKEFB");
        if (!fake || sscanf(fake, "%dx%dx%d", &fb.w, &fb.h, &bpp) != 3) {
            fprintf(stderr, "czdoom: %s is not a framebuffer device\n", path);
            exit(1);
        }
        fb.fake = 1;
        fb.stride = fb.w * (bpp / 8);
        if (bpp == 16) {
            fb.r_off = 11; fb.r_len = 5; fb.g_off = 5; fb.g_len = 6; fb.b_off = 0; fb.b_len = 5;
        } else {
            fb.r_off = 16; fb.r_len = 8; fb.g_off = 8; fb.g_len = 8; fb.b_off = 0; fb.b_len = 8;
        }
        fb.map_len = (size_t)fb.stride * fb.h;
        if (ftruncate(fb.fd, (off_t)fb.map_len) < 0) {
            fprintf(stderr, "czdoom: cannot size %s: %s\n", path, strerror(errno));
            exit(1);
        }
    }

    if ((bpp != 16 && bpp != 24 && bpp != 32) || fb.w <= 0 || fb.h <= 0 ||
        fb.r_len < 1 || fb.r_len > 8 || fb.g_len < 1 || fb.g_len > 8 || fb.b_len < 1 || fb.b_len > 8) {
        fprintf(stderr, "czdoom: unsupported framebuffer format (%dx%d, %d bpp)\n", fb.w, fb.h, bpp);
        exit(1);
    }
    fb.bytespp = bpp / 8;
    fb.is_rgb565 = (bpp == 16 && fb.r_off == 11 && fb.g_off == 5 && fb.b_off == 0 &&
                    fb.r_len == 5 && fb.g_len == 6 && fb.b_len == 5);

    fb.stage_len = (size_t)fb.stride * fb.h;
    fb.stage = calloc(1, fb.stage_len);

    fb.map = mmap(NULL, fb.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb.fd, 0);
    if (fb.map == MAP_FAILED || fb.origin + fb.stage_len > fb.map_len) {
        if (fb.map != MAP_FAILED) munmap(fb.map, fb.map_len);
        fb.map = NULL; /* fall back to pwrite() */
    }

    /* Default: fill the whole panel. CZDOOM_ASPECT=4:3 keeps Doom's original
     * proportions and pillarboxes instead. */
    fb.dx = 0; fb.dy = 0; fb.dw = fb.w; fb.dh = fb.h;
    const char *aspect = getenv("CZDOOM_ASPECT");
    if (aspect && strcmp(aspect, "4:3") == 0) {
        if (fb.h * 4 / 3 <= fb.w) {
            fb.dw = fb.h * 4 / 3;
            fb.dx = (fb.w - fb.dw) / 2;
        } else {
            fb.dh = fb.w * 3 / 4;
            fb.dy = (fb.h - fb.dh) / 2;
        }
    }

    build_axis(fb.dw, SRC_W, &x0_tab, &x1_tab, &xw_tab);
    build_axis(fb.dh, SRC_H, &y0_tab, &y1_tab, &yw_tab);

    printf("czdoom: video %s %dx%d %dbpp (%s), picture %dx%d at %d,%d\n", path, fb.w, fb.h, bpp,
           fb.map ? "mmap" : "pwrite", fb.dw, fb.dh, fb.dx, fb.dy);
    fb_blank();
}

void DG_DrawFrame(void)
{
    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;

    for (int y = 0; y < fb.dh; y++) {
        const uint32_t *ra = src + (size_t)y0_tab[y] * SRC_W;
        const uint32_t *row = ra;
        uint32_t wy = yw_tab[y];

        if (wy) {
            const uint32_t *rb = src + (size_t)y1_tab[y] * SRC_W;
            for (int x = 0; x < SRC_W; x++) row_tmp[x] = mix(ra[x], rb[x], wy);
            row = row_tmp;
        }

        uint8_t *out = fb.stage + (size_t)(fb.dy + y) * fb.stride + (size_t)fb.dx * fb.bytespp;
        for (int x = 0; x < fb.dw; x++) {
            uint32_t p = row[x0_tab[x]];
            if (xw_tab[x]) p = mix(p, row[x1_tab[x]], xw_tab[x]);

            if (fb.is_rgb565) {
                *(uint16_t *)(void *)out = (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
            } else {
                uint32_t v = pack(p);
                if (fb.bytespp == 2) {
                    *(uint16_t *)(void *)out = (uint16_t)v;
                } else if (fb.bytespp == 4) {
                    *(uint32_t *)(void *)out = v;
                } else {
                    out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8); out[2] = (uint8_t)(v >> 16);
                }
            }
            out += fb.bytespp;
        }
    }
    fb_flush();
}

/* ------------------------------------------------------------------ input */

#define MAX_KBD 8
#define KEYQ_LEN 64

static int kbd_fd[MAX_KBD];
static int kbd_count;
static uint16_t keyq[KEYQ_LEN];
static unsigned keyq_r, keyq_w;
static int literal_down;                 /* Alt held: letters are typed, not played */
static unsigned char sent[EK_MAX + 1];   /* what each held key was reported as */

static void keyq_push(int pressed, unsigned char key)
{
    unsigned next = (keyq_w + 1) % KEYQ_LEN;
    if (next == keyq_r) return; /* full: drop */
    keyq[keyq_w] = (uint16_t)((pressed ? 0x100 : 0) | key);
    keyq_w = next;
}

static unsigned char literal_key(int code)
{
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm", nums[] = "1234567890";
    if (code >= EK_1 && code <= EK_0) return (unsigned char)nums[code - EK_1];
    if (code >= EK_Q && code <= EK_P) return (unsigned char)row1[code - EK_Q];
    if (code >= EK_A && code <= EK_L) return (unsigned char)row2[code - EK_A];
    if (code >= EK_Z && code <= EK_M) return (unsigned char)row3[code - EK_Z];
    return 0;
}

static unsigned char translate(int code)
{
    switch (code) {
    case EK_ESC:        return KEY_ESCAPE;
    case EK_ENTER:
    case EK_KPENTER:    return KEY_ENTER;
    case EK_TAB:        return KEY_TAB;
    case EK_BACKSPACE:  return KEY_BACKSPACE;
    case EK_SPACE:      return KEY_USE;
    case EK_LEFTCTRL:
    case EK_RIGHTCTRL:  return KEY_FIRE;
    case EK_LEFTSHIFT:
    case EK_RIGHTSHIFT: return KEY_RSHIFT;
    case EK_UP:         return KEY_UPARROW;
    case EK_DOWN:       return KEY_DOWNARROW;
    case EK_LEFT:       return KEY_LEFTARROW;
    case EK_RIGHT:      return KEY_RIGHTARROW;
    case EK_MINUS:      return KEY_MINUS;
    case EK_EQUAL:      return KEY_EQUALS;
    case EK_COMMA:      return KEY_STRAFE_L;
    case EK_DOT:        return KEY_STRAFE_R;
    case EK_PAUSE:      return KEY_PAUSE;
    case EK_F11:        return KEY_F11;
    case EK_F12:        return KEY_F12;
    default: break;
    }
    if (code >= EK_F1 && code <= EK_F10) return (unsigned char)(KEY_F1 + (code - EK_F1));

    unsigned char ch = literal_key(code);
    if (!ch || literal_down) return ch;

    /* The built-in keyboard only reaches the arrow keys through Fn, so the
     * game is played on the letter keys. */
    switch (ch) {
    case 'w': return KEY_UPARROW;
    case 's': return KEY_DOWNARROW;
    case 'a': return KEY_LEFTARROW;
    case 'd': return KEY_RIGHTARROW;
    case 'q': return KEY_STRAFE_L;
    case 'e': return KEY_STRAFE_R;
    case 'k': return KEY_FIRE;
    case 'l': return KEY_USE;
    default:  return ch;
    }
}

static int is_keyboard(int fd)
{
    unsigned char bits[(EK_MAX + 8) / 8];
    struct stat st;

    /* A FIFO or plain file of raw events is accepted for off-device tests. */
    if (fstat(fd, &st) == 0 && !S_ISCHR(st.st_mode)) return 1;

    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVDEV_IOC_GBIT_KEY(sizeof(bits)), bits) < 0) return 0;
    /* must have both W and Enter */
    return (bits[17 / 8] & (1 << (17 % 8))) && (bits[EK_ENTER / 8] & (1 << (EK_ENTER % 8)));
}

static void kbd_add(const char *path)
{
    struct stat a, b;
    if (kbd_count >= MAX_KBD) return;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return;
    if (!is_keyboard(fd) || fstat(fd, &a) < 0) { close(fd); return; }
    for (int i = 0; i < kbd_count; i++) {
        /* same device reached through a symlink */
        if (fstat(kbd_fd[i], &b) == 0 && a.st_rdev == b.st_rdev && a.st_ino == b.st_ino) { close(fd); return; }
    }
    kbd_fd[kbd_count++] = fd;
    printf("czdoom: keyboard %s\n", path);
}

static void kbd_open(void)
{
    const char *env = getenv("CZDOOM_KBD");
    if (env && *env) {
        kbd_add(env);
    } else {
        kbd_add("/dev/input/cardputer-zero-internal");
        kbd_add("/dev/input/by-path/platform-3f804000.i2c-event");
        DIR *d = opendir("/dev/input");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                char path[300];
                if (strncmp(e->d_name, "event", 5) != 0) continue;
                snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
                kbd_add(path);
            }
            closedir(d);
        }
    }
    if (kbd_count == 0) {
        fprintf(stderr, "czdoom: no keyboard found under /dev/input (is this user in the 'input' group?)\n");
    }
}

static void kbd_poll(void)
{
    struct evdev_event ev[32];

    for (int i = 0; i < kbd_count; i++) {
        ssize_t n;
        while ((n = read(kbd_fd[i], ev, sizeof(ev))) >= (ssize_t)sizeof(ev[0])) {
            for (size_t k = 0; k < (size_t)n / sizeof(ev[0]); k++) {
                int code = ev[k].code;
                if (ev[k].type != EVDEV_EV_KEY || code > EK_MAX || ev[k].value == 2) continue;

                if (code == EK_LEFTALT || code == EK_RIGHTALT) {
                    literal_down = ev[k].value != 0;
                    continue;
                }
                if (ev[k].value) {
                    unsigned char key = translate(code);
                    if (!key) continue;
                    if (sent[code]) keyq_push(0, sent[code]);
                    sent[code] = key;
                    keyq_push(1, key);
                } else if (sent[code]) {
                    /* release whatever the press was reported as */
                    keyq_push(0, sent[code]);
                    sent[code] = 0;
                }
            }
        }
    }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (keyq_r == keyq_w) {
        kbd_poll();
        if (keyq_r == keyq_w) return 0;
    }
    uint16_t v = keyq[keyq_r];
    keyq_r = (keyq_r + 1) % KEYQ_LEN;
    *pressed = v >> 8;
    *key = (unsigned char)(v & 0xff);
    return 1;
}

/* ------------------------------------------------------------- lifecycle */

static volatile sig_atomic_t quit_requested;

static void on_signal(int sig)
{
    (void)sig;
    if (quit_requested) _exit(1); /* second signal: the game loop is stuck */
    quit_requested = 1;
}

/* doomgeneric's I_Quit() runs the engine's shutdown hooks (which save the
 * config) but never exits, so "Quit Game" would otherwise do nothing. Hooks
 * run newest first; this one is registered before the engine starts, so it
 * runs last. */
static void quit_after_shutdown(void)
{
    exit(0);
}

void DG_Init(void)
{
    extern int joybspeed;

    I_AtExit(quit_after_shutdown, false);

    /* Always run. Shift then walks instead. A saved default.cfg overrides it. */
    if (!getenv("CZDOOM_NO_AUTORUN")) joybspeed = 29;

    fb_open();
    kbd_open();
    atexit(fb_blank);
}

void DG_SleepMs(uint32_t ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv)
{
    struct sigaction sa;
    long max_frames = 0;
    const char *limit = getenv("CZDOOM_MAX_FRAMES"); /* testing aid */

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);   /* the launcher's Home long-press */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    if (limit) max_frames = atol(limit);

    doomgeneric_Create(argc, argv);

    for (long frame = 0; !quit_requested; frame++) {
        if (max_frames > 0 && frame >= max_frames) break;
        doomgeneric_Tick();
    }

    /* Leave without Doom's exit path so a signal never blocks on a prompt. */
    exit(0);
}
