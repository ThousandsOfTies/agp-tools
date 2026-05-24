/*
 * spi_shim.c — LD_PRELOAD shim for /dev/spidev0.0 (MFRC-522 simulation)
 *
 * Intercepts open()/ioctl() for SPI devices and simulates the MFRC-522
 * RFID reader register protocol. Card-present state is queried from the
 * bridge (/tmp/hw_sim.sock) so the HTML panel can trigger taps.
 *
 * Protocol (MFRC-522 SPI):
 *   Each transfer = 2 bytes
 *     byte0: address byte = (reg << 1) & 0x7E, MSB=1 for READ, 0 for WRITE
 *     byte1: data (write) or 0x00 (read), response in byte1
 */

#define _GNU_SOURCE
#include <sys/mman.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SIM_SOCK "/tmp/hw_sim.sock"
#define MAX_FDS  1024

/* ------------------------------------------------------------------ */
/* fd registry                                                          */
/* ------------------------------------------------------------------ */

static int  spi_fd_flag[MAX_FDS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* MFRC-522 register state                                              */
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

/* ------------------------------------------------------------------ */
/* Bridge connection (query card state)                                 */
/* ------------------------------------------------------------------ */

static int bridge_fd = -1;

static int bridge_connect(void) {
    if (bridge_fd >= 0) return bridge_fd;
    bridge_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bridge_fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SIM_SOCK, sizeof(addr.sun_path) - 1);
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
    if (write(s, req, strlen(req)) < 0) return 0;

    char resp[256] = {0};
    int n = read(s, resp, sizeof(resp) - 1);
    if (n <= 0) return 0;

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
    /* Parse UID hex bytes separated by ':' */
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

static void mfrc_init(void) {
    memset(regs, 0, sizeof(regs));
    fifo_w = fifo_r = 0;
    regs[VERSION_REG] = 0x92;        /* MFRC-522 v2.0 */
}

/* Called after a TRANSCEIVE command to simulate card response */
static void simulate_transceive(void) {
    /* Look at FIFO contents to determine what was sent */
    if (fifo_w == 0) {
        regs[COM_IRQ_REG] |= 0x01;   /* TimerIRq → no card */
        return;
    }

    uint8_t cmd_byte = fifo[0];

    /* Check card presence via bridge */
    uint8_t uid[4];
    int present = bridge_get_card(uid);
    fprintf(stderr, "[spi_shim] transceive cmd=0x%02x present=%d uid=%02X:%02X:%02X:%02X\n",
            cmd_byte, present, uid[0], uid[1], uid[2], uid[3]);

    if (!present) {
        regs[COM_IRQ_REG] |= 0x01;   /* TimerIRq → no card */
        fifo_r = fifo_w = 0;
        return;
    }

    /* Decide response based on first FIFO byte */
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
    return regs[reg];
}

static void reg_write(uint8_t reg, uint8_t val) {
    if (reg == COMMAND_REG) {
        regs[reg] = val & 0x0F;
        if ((val & 0x0F) == CMD_SOFT_RESET) {
            mfrc_init();
        } else if ((val & 0x0F) == CMD_TRANSCEIVE) {
            /* TRANSCEIVE will start once StartSend bit is set */
        } else if ((val & 0x0F) == CMD_IDLE) {
            /* idle */
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
/* Intercepted libc functions                                           */
/* ------------------------------------------------------------------ */

static int (*real_open)(const char *, int, ...)   = NULL;
static int (*real_open64)(const char *, int, ...) = NULL;
static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static int (*real_close)(int)                     = NULL;

__attribute__((constructor)) static void shim_init(void) {
    real_open   = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
    real_close  = dlsym(RTLD_NEXT, "close");
    mfrc_init();
    fprintf(stderr, "[spi_shim] loaded (MFRC-522 sim)\n");
}

static int is_spi_path(const char *p) {
    return strncmp(p, "/dev/spidev", 11) == 0;
}

int open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);

    if (is_spi_path(path)) {
        int fd = memfd_create("spi_sim", 0);
        if (fd >= 0 && fd < MAX_FDS) {
            pthread_mutex_lock(&mu);
            spi_fd_flag[fd] = 1;
            pthread_mutex_unlock(&mu);
            fprintf(stderr, "[spi_shim] open(%s) → fd=%d\n", path, fd);
        }
        return fd;
    }
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);

    if (is_spi_path(path)) {
        int fd = memfd_create("spi_sim", 0);
        if (fd >= 0 && fd < MAX_FDS) {
            pthread_mutex_lock(&mu);
            spi_fd_flag[fd] = 1;
            pthread_mutex_unlock(&mu);
        }
        return fd;
    }
    return real_open64(path, flags, mode);
}

int close(int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        pthread_mutex_lock(&mu);
        spi_fd_flag[fd] = 0;
        pthread_mutex_unlock(&mu);
    }
    return real_close(fd);
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *); va_end(ap);

    if (fd < 0 || fd >= MAX_FDS) return real_ioctl(fd, req, arg);

    pthread_mutex_lock(&mu);
    int is_spi = spi_fd_flag[fd];
    pthread_mutex_unlock(&mu);

    if (!is_spi) return real_ioctl(fd, req, arg);

    /* SPI_IOC_WR_MODE / SPI_IOC_WR_MAX_SPEED_HZ etc. — accept silently */
    if (((req >> 8) & 0xFF) == SPI_IOC_MAGIC && (req & 0x40000000UL) == 0
        && _IOC_NR(req) != 0) {
        return 0;
    }

    /* SPI_IOC_MESSAGE(N) */
    if (_IOC_TYPE(req) == SPI_IOC_MAGIC && _IOC_NR(req) == 0) {
        struct spi_ioc_transfer *tr = (struct spi_ioc_transfer *)arg;
        int n = _IOC_SIZE(req) / sizeof(struct spi_ioc_transfer);
        for (int i = 0; i < n; i++) {
            const uint8_t *tx = (const uint8_t *)(uintptr_t)tr[i].tx_buf;
            uint8_t       *rx = (uint8_t *)(uintptr_t)tr[i].rx_buf;
            int len = tr[i].len;
            /* MFRC-522 protocol: 2-byte transfers (addr, data) */
            if (len >= 2 && tx) {
                uint8_t addr_byte = tx[0];
                int is_read = (addr_byte & 0x80) != 0;
                uint8_t reg = (addr_byte >> 1) & 0x3F;
                if (is_read) {
                    if (rx) { rx[0] = 0; rx[1] = reg_read(reg); }
                } else {
                    reg_write(reg, tx[1]);
                    if (rx) memset(rx, 0, len);
                }
                /* For longer transfers, pad zeros */
                if (rx && len > 2) memset(rx + 2, 0, len - 2);
            } else if (rx) {
                memset(rx, 0, len);
            }
        }
        return n * 2;  /* roughly bytes transferred */
    }

    return real_ioctl(fd, req, arg);
}
