/*
 * cuse_i2c.c — CUSE-based I2C stub
 *
 * Creates /dev/i2c-1 (or the name given by --devname) as a userspace character
 * device.  Intercepts the three ioctl codes that the Linux i2c-dev interface
 * exposes to user-space programs:
 *
 *   I2C_SLAVE      – select the slave address for subsequent transfers
 *   I2C_RDWR       – combined read/write transaction (struct i2c_rdwr_ioctl_data)
 *   I2C_SMBUS      – SMBus transaction (struct i2c_smbus_ioctl_data)
 *
 * Simulated devices (extend the dispatch table in dispatch_i2c_addr()):
 *   0x29  VL53L0X  ToF distance sensor
 */

#define FUSE_USE_VERSION 31

#include <fuse3/cuse_lowlevel.h>
#include <fuse3/fuse_opt.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "vl53l0x_sim.h"
#include "ssd1306_sim.h"

/* ------------------------------------------------------------------ */
/* Per-open-file state                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t slave_addr;   /* last I2C_SLAVE address */
    uint8_t  reg_ptr;      /* last written register pointer */
    int      reg_ptr_set;
} i2c_session_t;

/* ------------------------------------------------------------------ */
/* Simulated device dispatch                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint16_t addr;
    uint8_t  (*read_reg)(uint8_t reg);
    void     (*write_reg)(uint8_t reg, uint8_t val);
    void     (*write_buf)(const uint8_t *buf, size_t len);  /* transaction-level (overrides write_reg) */
} sim_device_t;

static const sim_device_t sim_devices[] = {
    { VL53L0X_ADDR,    vl53l0x_sim_read_reg, vl53l0x_sim_write_reg, NULL },
    { SSD1306_SIM_ADDR, NULL,                NULL,                  ssd1306_sim_write },
    { 0, NULL, NULL, NULL }
};

static const sim_device_t *find_device(uint16_t addr) {
    for (int i = 0; sim_devices[i].addr != 0; i++) {
        if (sim_devices[i].addr == addr) return &sim_devices[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CUSE open / release                                                  */
/* ------------------------------------------------------------------ */

static void i2c_open(fuse_req_t req, struct fuse_file_info *fi) {
    i2c_session_t *s = calloc(1, sizeof(*s));
    if (!s) { fuse_reply_err(req, ENOMEM); return; }
    fi->fh = (uint64_t)(uintptr_t)s;
    fuse_reply_open(req, fi);
}

static void i2c_release(fuse_req_t req, struct fuse_file_info *fi) {
    free((void *)(uintptr_t)fi->fh);
    fuse_reply_err(req, 0);
}

/* ------------------------------------------------------------------ */
/* read / write (register-pointer style)                               */
/* ------------------------------------------------------------------ */

static void i2c_read(fuse_req_t req, size_t size, off_t off,
                     struct fuse_file_info *fi) {
    (void)off;
    i2c_session_t *s = (i2c_session_t *)(uintptr_t)fi->fh;
    const sim_device_t *dev = find_device(s->slave_addr);

    if (!dev || !dev->read_reg) { fuse_reply_err(req, ENXIO); return; }

    uint8_t *buf = malloc(size);
    if (!buf) { fuse_reply_err(req, ENOMEM); return; }

    for (size_t i = 0; i < size; i++) {
        buf[i] = dev->read_reg((uint8_t)(s->reg_ptr + i));
    }
    fuse_reply_buf(req, (char *)buf, size);
    free(buf);
}

static void i2c_write(fuse_req_t req, const char *buf, size_t size, off_t off,
                      struct fuse_file_info *fi) {
    (void)off;
    i2c_session_t *s = (i2c_session_t *)(uintptr_t)fi->fh;
    const sim_device_t *dev = find_device(s->slave_addr);

    if (!dev || size == 0) { fuse_reply_err(req, ENXIO); return; }

    if (dev->write_buf) {
        /* Transaction-level write (e.g. SSD1306) */
        dev->write_buf((const uint8_t *)buf, size);
    } else if (dev->write_reg) {
        /* Register-pointer style: first byte = register address */
        s->reg_ptr = (uint8_t)buf[0];
        s->reg_ptr_set = 1;
        for (size_t i = 1; i < size; i++) {
            dev->write_reg((uint8_t)(s->reg_ptr + (i - 1)), (uint8_t)buf[i]);
        }
    }
    fuse_reply_write(req, size);
}

/* ------------------------------------------------------------------ */
/* ioctl                                                               */
/* ------------------------------------------------------------------ */

static void handle_i2c_rdwr(fuse_req_t req, struct fuse_file_info *fi,
                             const struct i2c_rdwr_ioctl_data *arg,
                             const void *in_buf, size_t in_bufsz) {
    /*
     * CUSE ioctl retry protocol: if FUSE_IOCTL_RETRY is not set yet we must
     * request the full iovec covering the msgs array and each msg buf.
     * We use the simplified approach: ask for the full in_buf upfront.
     *
     * The kernel passes us a flat buffer after retries resolve the pointers.
     * We reconstruct the msgs from it here.
     */

    i2c_session_t *s = (i2c_session_t *)(uintptr_t)fi->fh;

    if (in_bufsz < sizeof(struct i2c_rdwr_ioctl_data)) {
        /* Ask for the header first */
        struct iovec iov = { (void *)(uintptr_t)arg,
                             sizeof(struct i2c_rdwr_ioctl_data) };
        fuse_reply_ioctl_retry(req, &iov, 1, NULL, 0);
        return;
    }

    const struct i2c_rdwr_ioctl_data *data = in_buf;
    uint32_t nmsgs = data->nmsgs;

    /*
     * Build the iovec list for retry: header + msgs array + each data buffer.
     * Maximum I2C_RDWR_IOCTL_MAX_MSGS = 42 per kernel.
     */
    if (nmsgs == 0 || nmsgs > 42) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    size_t msgs_size = nmsgs * sizeof(struct i2c_msg);
    size_t needed = sizeof(struct i2c_rdwr_ioctl_data) + msgs_size;
    /* Add each message buffer size */
    const struct i2c_msg *msgs = (const struct i2c_msg *)(data + 1);
    for (uint32_t i = 0; i < nmsgs && in_bufsz >= needed + sizeof(struct i2c_msg); i++) {
        needed += msgs[i].len;
    }

    if (in_bufsz < needed) {
        /* Not enough data yet, build retry iovecs */
        struct iovec in_iov[43 * 2 + 1];
        int iov_cnt = 0;

        in_iov[iov_cnt].iov_base = (void *)(uintptr_t)arg;
        in_iov[iov_cnt].iov_len  = sizeof(struct i2c_rdwr_ioctl_data);
        iov_cnt++;

        if (in_bufsz >= sizeof(struct i2c_rdwr_ioctl_data)) {
            in_iov[iov_cnt].iov_base = (void *)(uintptr_t)data->msgs;
            in_iov[iov_cnt].iov_len  = msgs_size;
            iov_cnt++;

            if (in_bufsz >= sizeof(struct i2c_rdwr_ioctl_data) + msgs_size) {
                for (uint32_t i = 0; i < nmsgs; i++) {
                    in_iov[iov_cnt].iov_base = (void *)(uintptr_t)msgs[i].buf;
                    in_iov[iov_cnt].iov_len  = msgs[i].len;
                    iov_cnt++;
                }
            }
        }
        fuse_reply_ioctl_retry(req, in_iov, iov_cnt, NULL, 0);
        return;
    }

    /* All data available — process each message */
    const uint8_t *cursor = (const uint8_t *)(msgs + nmsgs);
    uint8_t out_bufs[42][256];
    int     out_lens[42];
    int     have_out = 0;

    for (uint32_t i = 0; i < nmsgs; i++) {
        uint16_t addr = msgs[i].addr;
        uint16_t len  = msgs[i].len;
        const sim_device_t *dev = find_device(addr);

        s->slave_addr = addr;

        if (msgs[i].flags & I2C_M_RD) {
            /* Read message */
            out_lens[i] = len;
            have_out = 1;
            if (dev && dev->read_reg) {
                for (int j = 0; j < len; j++) {
                    out_bufs[i][j] = dev->read_reg((uint8_t)(s->reg_ptr + j));
                }
            } else {
                memset(out_bufs[i], 0xFF, len);
            }
            cursor += len;
        } else {
            /* Write message */
            out_lens[i] = 0;
            if (len > 0 && dev) {
                if (dev->write_buf) {
                    dev->write_buf(cursor, len);
                } else if (dev->write_reg) {
                    s->reg_ptr = cursor[0];
                    for (int j = 1; j < len; j++) {
                        dev->write_reg((uint8_t)(s->reg_ptr + (j - 1)), cursor[j]);
                    }
                }
            }
            cursor += len;
        }
    }

    if (have_out) {
        /* Pack read results into output buffer */
        uint8_t out[42 * 256];
        size_t out_pos = 0;
        for (uint32_t i = 0; i < nmsgs; i++) {
            if (out_lens[i] > 0) {
                memcpy(out + out_pos, out_bufs[i], out_lens[i]);
                out_pos += out_lens[i];
            }
        }
        fuse_reply_ioctl(req, 0, out, out_pos);
    } else {
        fuse_reply_ioctl(req, 0, NULL, 0);
    }
}

static void i2c_ioctl(fuse_req_t req, int cmd, void *arg,
                      struct fuse_file_info *fi, unsigned flags,
                      const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    (void)out_bufsz;

    i2c_session_t *s = (i2c_session_t *)(uintptr_t)fi->fh;

    switch ((unsigned long)cmd) {

    case I2C_SLAVE:
    case I2C_SLAVE_FORCE:
        s->slave_addr = (uint16_t)(uintptr_t)arg;
        fprintf(stderr, "[cuse_i2c] I2C_SLAVE addr=0x%02x\n", s->slave_addr);
        fuse_reply_ioctl(req, 0, NULL, 0);
        break;

    case I2C_RDWR:
        handle_i2c_rdwr(req, fi,
                        (const struct i2c_rdwr_ioctl_data *)(uintptr_t)arg,
                        in_buf, in_bufsz);
        break;

    case I2C_FUNCS: {
        /* Report supported functionality */
        if (!(flags & FUSE_IOCTL_DIR)) {
            /* Need to write output */
            unsigned long funcs = I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
            struct iovec out_iov = { arg, sizeof(funcs) };
            fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
        } else {
            unsigned long funcs = I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
            fuse_reply_ioctl(req, 0, &funcs, sizeof(funcs));
        }
        break;
    }

    default:
        fprintf(stderr, "[cuse_i2c] unknown ioctl 0x%x\n", cmd);
        fuse_reply_err(req, ENOTTY);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* CUSE ops table                                                       */
/* ------------------------------------------------------------------ */

static const struct cuse_lowlevel_ops i2c_clops = {
    .open    = i2c_open,
    .release = i2c_release,
    .read    = i2c_read,
    .write   = i2c_write,
    .ioctl   = i2c_ioctl,
};

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[]) {
    const char *devname = "i2c-1";

    /* Parse --devname= manually to avoid fuse_opt_parse heap issues */
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

    vl53l0x_sim_init();
    ssd1306_sim_init();

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

    fprintf(stderr, "[cuse_i2c] starting /dev/%s stub\n", devname);
    int ret = cuse_lowlevel_main(args.argc, args.argv, &ci, &i2c_clops, NULL);
    free(fuse_argv);
    return ret;
}
