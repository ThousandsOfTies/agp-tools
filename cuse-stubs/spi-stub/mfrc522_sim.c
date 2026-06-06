/*
 * mfrc522_sim.c — MFRC-522 register simulation for the CUSE SPI stub.
 *
 * Ported from spi_shim.c (LD_PRELOAD). The register/FIFO state machine is
 * unchanged; only the entry point differs: instead of intercepting
 * SPI_IOC_MESSAGE inside the application, cuse_spi feeds each transfer here
 * via mfrc522_sim_transfer().
 *
 * Card-present state is read from the web bridge socket so the HTML panel /
 * `agp sim ui rfid tap` can trigger taps.
 */

#include "mfrc522_sim.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* MFRC-522 register map                                                */
/* ------------------------------------------------------------------ */

#define COMMAND_REG     0x01
#define COM_IRQ_REG     0x04
#define DIV_IRQ_REG     0x05
#define ERROR_REG       0x06
#define FIFO_DATA_REG   0x09
#define FIFO_LEVEL_REG  0x0A
#define CONTROL_REG     0x0C
#define BIT_FRAMING_REG 0x0D
#define MODE_REG        0x11
#define TX_CONTROL_REG  0x14
#define VERSION_REG     0x37

#define CMD_IDLE        0x00
#define CMD_TRANSCEIVE  0x0C
#define CMD_SOFT_RESET  0x0F

#define PICC_REQA       0x26
#define PICC_ANTICOLL   0x93

static uint8_t regs[64];
static uint8_t fifo[64];
static int     fifo_w = 0, fifo_r = 0;
static uint8_t last_uid[4] = {0};
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Bridge connection (query card state)                                 */
/* ------------------------------------------------------------------ */

static int bridge_fd = -1;

static const char *bridge_socket_path(void) {
    const char *explicit_path = getenv("AGP_HW_SIM_SOCK");
    if (explicit_path && explicit_path[0]) {
        return explicit_path;
    }

    const char *runtime_dir = getenv("AGP_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0]) {
        static char path[108];
        snprintf(path, sizeof(path), "%s/hw_sim.sock", runtime_dir);
        return path;
    }

    return "/tmp/hw_sim.sock";
}

static int bridge_connect(void) {
    if (bridge_fd >= 0) return bridge_fd;
    bridge_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bridge_fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    const char *sock_path = bridge_socket_path();
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        close(bridge_fd);
        bridge_fd = -1;
        return -1;
    }
    strcpy(addr.sun_path, sock_path);
    if (connect(bridge_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(bridge_fd);
        bridge_fd = -1;
    }
    return bridge_fd;
}

/* Returns 1 if card present (sets uid_out), 0 otherwise */
static int bridge_get_card(uint8_t *uid_out) {
    int s = bridge_connect();
    if (s < 0) return 0;
    const char *req = "{\"req\":\"get\",\"device\":\"rfid\"}\n";
    if (write(s, req, strlen(req)) < 0) {
        close(bridge_fd);
        bridge_fd = -1;
        return 0;
    }

    char resp[256] = {0};
    int n = read(s, resp, sizeof(resp) - 1);
    if (n <= 0) {
        close(bridge_fd);
        bridge_fd = -1;
        return 0;
    }

    /* Parse "present": true and "uid": "XX:XX:XX:XX" (handle whitespace) */
    char *p = strstr(resp, "\"present\"");
    if (!p) return 0;
    p += 9;
    while (*p == ' ' || *p == ':') p++;
    if (strncmp(p, "true", 4) != 0) return 0;

    p = strstr(resp, "\"uid\"");
    if (!p) return 0;
    p += 5;
    while (*p == ' ' || *p == ':' || *p == '"') p++;
    for (int i = 0; i < 4; i++) {
        if (!*p) return 0;
        unsigned int v = 0;
        if (sscanf(p, "%2x", &v) != 1) return 0;
        uid_out[i] = (uint8_t)v;
        p += 2;
        if (*p == ':') p++;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* MFRC-522 register simulation                                         */
/* ------------------------------------------------------------------ */

static void mfrc_reset(void) {
    memset(regs, 0, sizeof(regs));
    fifo_w = fifo_r = 0;
    regs[VERSION_REG] = 0x92;        /* MFRC-522 v2.0 */
}

/* Called after a TRANSCEIVE command to simulate card response */
static void simulate_transceive(void) {
    if (fifo_w == 0) {
        regs[COM_IRQ_REG] |= 0x01;   /* TimerIRq → no card */
        return;
    }

    uint8_t cmd_byte = fifo[0];

    uint8_t uid[4];
    int present = bridge_get_card(uid);
    fprintf(stderr, "[cuse_spi] transceive cmd=0x%02x present=%d uid=%02X:%02X:%02X:%02X\n",
            cmd_byte, present, uid[0], uid[1], uid[2], uid[3]);

    if (!present) {
        regs[COM_IRQ_REG] |= 0x01;   /* TimerIRq → no card */
        fifo_r = fifo_w = 0;
        return;
    }

    uint8_t cmd = cmd_byte;
    fifo_r = fifo_w = 0;

    if (cmd == PICC_REQA) {
        /* ATQA = 0x0044 (2 bytes, 16 bits) */
        fifo[fifo_w++] = 0x44;
        fifo[fifo_w++] = 0x00;
        regs[FIFO_LEVEL_REG] = 2;
        regs[CONTROL_REG] = 0;        /* full bytes */
        regs[COM_IRQ_REG] |= 0x30;   /* RxIRq | IdleIRq */
        memcpy(last_uid, uid, 4);
    } else if (cmd == PICC_ANTICOLL) {
        /* UID (4) + BCC (1) = 5 bytes, 40 bits */
        memcpy(fifo, last_uid, 4);
        fifo[4] = last_uid[0] ^ last_uid[1] ^ last_uid[2] ^ last_uid[3];
        fifo_w = 5;
        regs[FIFO_LEVEL_REG] = 5;
        regs[CONTROL_REG] = 0;
        regs[COM_IRQ_REG] |= 0x30;
    } else {
        regs[COM_IRQ_REG] |= 0x01;
    }
}

static uint8_t reg_read(uint8_t reg) {
    if (reg == FIFO_DATA_REG) {
        if (fifo_r < fifo_w) {
            uint8_t v = fifo[fifo_r++];
            if (regs[FIFO_LEVEL_REG] > 0) regs[FIFO_LEVEL_REG]--;
            return v;
        }
        return 0;
    }
    return regs[reg & 0x3F];
}

static void reg_write(uint8_t reg, uint8_t val) {
    reg &= 0x3F;
    if (reg == COMMAND_REG) {
        regs[reg] = val & 0x0F;
        if ((val & 0x0F) == CMD_SOFT_RESET) {
            mfrc_reset();
        }
        return;
    }
    if (reg == FIFO_DATA_REG) {
        if (fifo_w < (int)sizeof(fifo)) {
            fifo[fifo_w++] = val;
            regs[FIFO_LEVEL_REG] = fifo_w;
        }
        return;
    }
    if (reg == FIFO_LEVEL_REG) {
        if (val & 0x80) { fifo_w = fifo_r = 0; regs[FIFO_LEVEL_REG] = 0; }
        return;
    }
    if (reg == BIT_FRAMING_REG) {
        regs[reg] = val;
        if ((val & 0x80) && regs[COMMAND_REG] == CMD_TRANSCEIVE) {
            simulate_transceive();
        }
        return;
    }
    regs[reg] = val;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void mfrc522_sim_init(void) {
    pthread_mutex_lock(&mu);
    mfrc_reset();
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "[cuse_spi] MFRC-522 sim initialised (VersionReg=0x92)\n");
}

void mfrc522_sim_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (rx) memset(rx, 0, len);
    if (len < 2) return;

    uint8_t addr_byte = tx ? tx[0] : 0;
    int is_read = (addr_byte & 0x80) != 0;
    uint8_t reg = (addr_byte >> 1) & 0x3F;

    pthread_mutex_lock(&mu);
    if (is_read) {
        uint8_t v = reg_read(reg);
        if (rx) rx[1] = v;
    } else {
        reg_write(reg, tx ? tx[1] : 0);
    }
    pthread_mutex_unlock(&mu);
}
