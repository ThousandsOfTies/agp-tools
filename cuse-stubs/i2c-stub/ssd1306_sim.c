/*
 * ssd1306_sim.c — SSD1306 OLED simulation for cuse_i2c
 *
 * Captures I2C writes to address 0x3C, parses the SSD1306 protocol,
 * maintains a 128x64 framebuffer, and pushes updates to the web bridge
 * via /tmp/hw_sim.sock.
 *
 * SSD1306 I2C protocol (after slave address):
 *   Each transaction starts with a control byte:
 *     0x00 = next bytes are COMMANDS
 *     0x40 = next bytes are DATA (pixels to GDDRAM)
 *   Commands: addressing mode, cursor position, etc.
 *   Data goes to GDDRAM at current cursor position.
 */

#include "ssd1306_sim.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define WIDTH  128
#define HEIGHT 64
#define PAGES  (HEIGHT / 8)

static uint8_t fb[WIDTH * PAGES];     /* page-major: fb[page*128 + col] */
static int     col_start = 0, col_end = WIDTH - 1;
static int     page_start = 0, page_end = PAGES - 1;
static int     cur_col = 0, cur_page = 0;

static int     bridge_fd = -1;

/* ------------------------------------------------------------------ */
/* Bridge connection                                                   */
/* ------------------------------------------------------------------ */

static int bridge_connect(void) {
    if (bridge_fd >= 0) return bridge_fd;
    bridge_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bridge_fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, "/tmp/hw_sim.sock", sizeof(addr.sun_path) - 1);
    if (connect(bridge_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(bridge_fd);
        bridge_fd = -1;
    }
    return bridge_fd;
}

/* base64 encode for transmission to bridge */
static const char b64chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, size_t in_len, char *out) {
    size_t i, o = 0;
    for (i = 0; i + 2 < in_len; i += 3) {
        out[o++] = b64chars[(in[i] >> 2) & 0x3F];
        out[o++] = b64chars[((in[i] & 0x03) << 4) | ((in[i + 1] >> 4) & 0x0F)];
        out[o++] = b64chars[((in[i + 1] & 0x0F) << 2) | ((in[i + 2] >> 6) & 0x03)];
        out[o++] = b64chars[in[i + 2] & 0x3F];
    }
    if (i < in_len) {
        out[o++] = b64chars[(in[i] >> 2) & 0x3F];
        if (i + 1 < in_len) {
            out[o++] = b64chars[((in[i] & 0x03) << 4) | ((in[i + 1] >> 4) & 0x0F)];
            out[o++] = b64chars[(in[i + 1] & 0x0F) << 2];
        } else {
            out[o++] = b64chars[(in[i] & 0x03) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = '\0';
}

static void send_framebuffer(void) {
    int s = bridge_connect();
    if (s < 0) return;

    /* fb is 1024 bytes → b64 ≈ 1368 chars */
    char b64[2048];
    b64_encode(fb, sizeof(fb), b64);

    char msg[2200];
    int n = snprintf(msg, sizeof(msg),
        "{\"event\":\"set\",\"device\":\"oled\",\"framebuf\":\"%s\"}\n", b64);
    if (write(s, msg, n) < 0) {
        /* drop on error */
    }
}

/* ------------------------------------------------------------------ */
/* SSD1306 logic                                                       */
/* ------------------------------------------------------------------ */

void ssd1306_sim_init(void) {
    memset(fb, 0, sizeof(fb));
    col_start = 0; col_end = WIDTH - 1;
    page_start = 0; page_end = PAGES - 1;
    cur_col = 0; cur_page = 0;
}

static void process_commands(const uint8_t *cmds, size_t len) {
    /* Minimal command decoder — only what we need for framebuffer tracking */
    size_t i = 0;
    while (i < len) {
        uint8_t c = cmds[i++];
        if (c == 0x21 && i + 1 < len) {        /* set column address */
            col_start = cmds[i++];
            col_end   = cmds[i++];
            cur_col   = col_start;
        } else if (c == 0x22 && i + 1 < len) { /* set page address */
            page_start = cmds[i++];
            page_end   = cmds[i++];
            cur_page   = page_start;
        }
        /* Other commands ignored — we don't need to track display on/off,
           contrast, etc. for visualization. */
    }
}

static void process_data(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (cur_page < PAGES && cur_col < WIDTH) {
            fb[cur_page * WIDTH + cur_col] = data[i];
        }
        cur_col++;
        if (cur_col > col_end) {
            cur_col = col_start;
            cur_page++;
            if (cur_page > page_end) cur_page = page_start;
        }
    }
    send_framebuffer();
}

void ssd1306_sim_write(const uint8_t *buf, size_t len) {
    if (len < 1) return;
    uint8_t ctrl = buf[0];
    if (ctrl == 0x00) {
        process_commands(buf + 1, len - 1);
    } else if (ctrl == 0x40) {
        process_data(buf + 1, len - 1);
    } else {
        fprintf(stderr, "[ssd1306_sim] unknown control byte 0x%02x\n", ctrl);
    }
}
