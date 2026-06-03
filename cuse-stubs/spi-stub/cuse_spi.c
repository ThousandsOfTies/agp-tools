/*
 * cuse_spi.c — CUSE-based SPI stub (/dev/spidev0.0)
 *
 * Creates /dev/spidev0.0 (or the name given by --devname) as a userspace
 * character device and serves the spidev ioctl interface used by the
 * MFRC-522 RFID driver:
 *
 *   SPI_IOC_RD/WR_MODE          – SPI mode (CPOL/CPHA)            (accepted)
 *   SPI_IOC_RD/WR_LSB_FIRST     – bit order                        (accepted)
 *   SPI_IOC_RD/WR_BITS_PER_WORD – word size                        (accepted)
 *   SPI_IOC_RD/WR_MAX_SPEED_HZ  – clock speed                      (accepted)
 *   SPI_IOC_RD/WR_MODE32        – 32-bit mode                      (accepted)
 *   SPI_IOC_MESSAGE(N)          – full-duplex transfer array       (simulated)
 *
 * This replaces the LD_PRELOAD spi_shim: instead of intercepting ioctl()
 * inside the application, the kernel routes spidev ioctls to us through
 * /dev/fuse, so the unmodified ~/sensor_demo binary talks to a real device
 * node. The MFRC-522 register state machine lives in mfrc522_sim.c.
 *
 * ARM build from this subdirectory: make CC=aarch64-linux-gnu-gcc
 */

#define FUSE_USE_VERSION 31

#include <fuse3/cuse_lowlevel.h>
#include <fuse3/fuse_opt.h>

#include <linux/spi/spidev.h>

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "mfrc522_sim.h"

#define MAX_XFERS  64

/* ------------------------------------------------------------------ */
/* Device configuration (accepted from the driver, echoed back)        */
/* ------------------------------------------------------------------ */

static uint8_t  spi_mode  = 0;
static uint8_t  spi_lsb   = 0;
static uint8_t  spi_bits  = 8;
static uint32_t spi_speed = 1000000;
static uint32_t spi_mode32 = 0;

/* ------------------------------------------------------------------ */
/* CUSE open / release                                                  */
/* ------------------------------------------------------------------ */

static void spi_open(fuse_req_t req, struct fuse_file_info *fi) {
    fuse_reply_open(req, fi);
}

static void spi_release(fuse_req_t req, struct fuse_file_info *fi) {
    (void)fi;
    fuse_reply_err(req, 0);
}

/* ------------------------------------------------------------------ */
/* Fixed-size scalar ioctls (RD_* / WR_*)                              */
/* ------------------------------------------------------------------ */

static void scalar_rd(fuse_req_t req, void *arg, size_t out_bufsz,
                      const void *val, size_t sz) {
    if (out_bufsz == 0) {
        struct iovec out = { arg, sz };
        fuse_reply_ioctl_retry(req, NULL, 0, &out, 1);
        return;
    }
    fuse_reply_ioctl(req, 0, val, sz);
}

static void scalar_wr(fuse_req_t req, void *arg, const void *in_buf,
                      size_t in_bufsz, void *dst, size_t sz) {
    if (in_bufsz < sz) {
        struct iovec in = { arg, sz };
        fuse_reply_ioctl_retry(req, &in, 1, NULL, 0);
        return;
    }
    memcpy(dst, in_buf, sz);
    fuse_reply_ioctl(req, 0, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* SPI_IOC_MESSAGE(N) — full-duplex transfer array                     */
/* ------------------------------------------------------------------ */

static void handle_spi_message(fuse_req_t req, int cmd, void *arg,
                               const void *in_buf, size_t in_bufsz,
                               size_t out_bufsz) {
    (void)out_bufsz;
    size_t arr_size = _IOC_SIZE((unsigned)cmd);
    int n = (int)(arr_size / sizeof(struct spi_ioc_transfer));
    if (n <= 0 || n > MAX_XFERS) { fuse_reply_err(req, EINVAL); return; }

    /* Phase A: fetch the transfer descriptor array. */
    if (in_bufsz < arr_size) {
        struct iovec in = { arg, arr_size };
        fuse_reply_ioctl_retry(req, &in, 1, NULL, 0);
        return;
    }

    const struct spi_ioc_transfer *tr = in_buf;

    /* How many tx bytes follow the array once all pointers are resolved? */
    size_t tx_total = 0;
    for (int i = 0; i < n; i++) {
        if (tr[i].tx_buf) tx_total += tr[i].len;
    }

    /* Phase B: gather tx buffers (in) and rx buffers (out) via retry. */
    if (in_bufsz < arr_size + tx_total) {
        struct iovec in_iov[1 + MAX_XFERS];
        struct iovec out_iov[MAX_XFERS];
        int ic = 0, oc = 0;

        in_iov[ic].iov_base = arg;
        in_iov[ic].iov_len  = arr_size;
        ic++;
        for (int i = 0; i < n; i++) {
            if (tr[i].tx_buf) {
                in_iov[ic].iov_base = (void *)(uintptr_t)tr[i].tx_buf;
                in_iov[ic].iov_len  = tr[i].len;
                ic++;
            }
        }
        for (int i = 0; i < n; i++) {
            if (tr[i].rx_buf) {
                out_iov[oc].iov_base = (void *)(uintptr_t)tr[i].rx_buf;
                out_iov[oc].iov_len  = tr[i].len;
                oc++;
            }
        }
        fuse_reply_ioctl_retry(req, in_iov, ic, oc ? out_iov : NULL, oc);
        return;
    }

    /* Phase C: all tx data present — run the transfers. */
    const uint8_t *tx_cursor = (const uint8_t *)in_buf + arr_size;

    size_t rx_total = 0;
    size_t total_len = 0;
    for (int i = 0; i < n; i++) {
        if (tr[i].rx_buf) rx_total += tr[i].len;
        total_len += tr[i].len;
    }

    uint8_t *out = rx_total ? malloc(rx_total) : NULL;
    if (rx_total && !out) { fuse_reply_err(req, ENOMEM); return; }
    size_t out_pos = 0;

    for (int i = 0; i < n; i++) {
        size_t len = tr[i].len;
        const uint8_t *tx = tr[i].tx_buf ? tx_cursor : NULL;
        uint8_t rxbuf[260];
        uint8_t *rx = NULL;

        if (tr[i].rx_buf) {
            rx = (len <= sizeof(rxbuf)) ? rxbuf : malloc(len);
            if (!rx) { free(out); fuse_reply_err(req, ENOMEM); return; }
        }

        mfrc522_sim_transfer(tx, rx, len);

        if (tr[i].rx_buf && out) {
            memcpy(out + out_pos, rx, len);
            out_pos += len;
        }
        if (rx && rx != rxbuf) free(rx);
        if (tr[i].tx_buf) tx_cursor += len;
    }

    fuse_reply_ioctl(req, (int)total_len, out, out_pos);
    free(out);
}

/* ------------------------------------------------------------------ */
/* ioctl dispatch                                                      */
/* ------------------------------------------------------------------ */

static void spi_ioctl(fuse_req_t req, int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned flags,
                      const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    (void)fi;
    (void)flags;

    if (_IOC_TYPE((unsigned)cmd) != SPI_IOC_MAGIC) {
        fuse_reply_err(req, ENOTTY);
        return;
    }

    unsigned nr = _IOC_NR((unsigned)cmd);

    if (nr == 0) {                       /* SPI_IOC_MESSAGE(N) */
        handle_spi_message(req, cmd, arg, in_buf, in_bufsz, out_bufsz);
        return;
    }

    int is_read = (_IOC_DIR((unsigned)cmd) & _IOC_READ) != 0;

    switch (nr) {
    case 1: /* MODE */
        if (is_read) scalar_rd(req, arg, out_bufsz, &spi_mode, sizeof(spi_mode));
        else         scalar_wr(req, arg, in_buf, in_bufsz, &spi_mode, sizeof(spi_mode));
        break;
    case 2: /* LSB_FIRST */
        if (is_read) scalar_rd(req, arg, out_bufsz, &spi_lsb, sizeof(spi_lsb));
        else         scalar_wr(req, arg, in_buf, in_bufsz, &spi_lsb, sizeof(spi_lsb));
        break;
    case 3: /* BITS_PER_WORD */
        if (is_read) scalar_rd(req, arg, out_bufsz, &spi_bits, sizeof(spi_bits));
        else         scalar_wr(req, arg, in_buf, in_bufsz, &spi_bits, sizeof(spi_bits));
        break;
    case 4: /* MAX_SPEED_HZ */
        if (is_read) scalar_rd(req, arg, out_bufsz, &spi_speed, sizeof(spi_speed));
        else         scalar_wr(req, arg, in_buf, in_bufsz, &spi_speed, sizeof(spi_speed));
        break;
    case 5: /* MODE32 */
        if (is_read) scalar_rd(req, arg, out_bufsz, &spi_mode32, sizeof(spi_mode32));
        else         scalar_wr(req, arg, in_buf, in_bufsz, &spi_mode32, sizeof(spi_mode32));
        break;
    default:
        fprintf(stderr, "[cuse_spi] unknown ioctl nr=%u\n", nr);
        fuse_reply_err(req, ENOTTY);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* CUSE ops table                                                       */
/* ------------------------------------------------------------------ */

static const struct cuse_lowlevel_ops spi_clops = {
    .open    = spi_open,
    .release = spi_release,
    .ioctl   = spi_ioctl,
};

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    const char *devname = "spidev0.0";

    char **fuse_argv = malloc((argc + 1) * sizeof(char *));
    if (!fuse_argv) return 1;
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--devname=", 10) == 0) {
            devname = argv[i] + 10;
        } else {
            fuse_argv[fuse_argc++] = argv[i];
        }
    }
    fuse_argv[fuse_argc] = NULL;

    struct fuse_args args = FUSE_ARGS_INIT(fuse_argc, fuse_argv);

    mfrc522_sim_init();

    char dev_name_buf[64];
    snprintf(dev_name_buf, sizeof(dev_name_buf), "DEVNAME=%s", devname);
    const char *dev_info_argv[] = { dev_name_buf };

    struct cuse_info ci = {
        .dev_major      = 0,
        .dev_minor      = 0,
        .dev_info_argc  = 1,
        .dev_info_argv  = dev_info_argv,
        .flags          = CUSE_UNRESTRICTED_IOCTL,
    };

    fprintf(stderr, "[cuse_spi] starting /dev/%s stub\n", devname);
    int ret = cuse_lowlevel_main(args.argc, args.argv, &ci, &spi_clops, NULL);
    free(fuse_argv);
    return ret;
}
