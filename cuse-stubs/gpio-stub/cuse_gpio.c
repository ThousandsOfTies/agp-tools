/*
 * cuse_gpio.c — experimental CUSE-based GPIO chip stub
 *
 * Creates /dev/gpiochip0 (or the name given by --devname) as a userspace
 * character device. It implements enough GPIO chardev metadata/value ioctls to
 * validate the web-bridge protocol, but it cannot transparently emulate line
 * request fd creation. See README.md in this directory.
 */

#define FUSE_USE_VERSION 31

#include <fuse3/cuse_lowlevel.h>
#include <fuse3/fuse_opt.h>

#include <linux/gpio.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SIM_SOCK "/tmp/hw_sim.sock"
#define GPIO_LINES 54
#define MAX_TRACKED_LINES 16

typedef struct {
    unsigned int offset;
    const char *name;
    int is_output;
} line_def_t;

static const line_def_t line_defs[] = {
    {17, "BTN_GPIO17", 0},
    {18, "LED_GPIO18", 1},
    {24, "LED_GPIO24", 1},
    {27, "BTN_GPIO27", 0},
    {0, NULL, 0},
};

typedef struct {
    unsigned int output_lines[MAX_TRACKED_LINES];
    unsigned int input_lines[MAX_TRACKED_LINES];
    unsigned int output_count;
    unsigned int input_count;
} gpio_session_t;

static int bridge_fd = -1;

static const line_def_t *find_line(unsigned int offset) {
    for (int i = 0; line_defs[i].name != NULL; i++) {
        if (line_defs[i].offset == offset) {
            return &line_defs[i];
        }
    }
    return NULL;
}

static int bridge_connect(void) {
    if (bridge_fd >= 0) {
        return bridge_fd;
    }

    bridge_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bridge_fd < 0) {
        return -1;
    }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SIM_SOCK, sizeof(addr.sun_path) - 1);
    if (connect(bridge_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(bridge_fd);
        bridge_fd = -1;
        return -1;
    }

    return bridge_fd;
}

static void bridge_send(const char *json_line) {
    int s = bridge_connect();
    if (s < 0) {
        fprintf(stderr, "[cuse_gpio] bridge not available: %s\n", json_line);
        return;
    }
    if (write(s, json_line, strlen(json_line)) < 0) {
        fprintf(stderr, "[cuse_gpio] bridge_send failed\n");
    }
}

static int bridge_get_button(unsigned int line) {
    int s = bridge_connect();
    if (s < 0) {
        return 0;
    }

    char req[128];
    snprintf(req, sizeof(req),
             "{\"req\":\"get\",\"device\":\"gpio\",\"line\":%u}\n", line);
    if (write(s, req, strlen(req)) < 0) {
        return 0;
    }

    char resp[128] = {0};
    ssize_t n = read(s, resp, sizeof(resp) - 1);
    if (n <= 0) {
        return 0;
    }

    char *p = strstr(resp, "\"value\":");
    return p ? atoi(p + 8) : 0;
}

static void register_line(unsigned int line, const char *dir) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"event\":\"register\",\"device\":\"gpio\","
             "\"line\":%u,\"dir\":\"%s\"}\n",
             line, dir);
    bridge_send(msg);
}

static void set_line_value(unsigned int line, int value) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "{\"event\":\"set\",\"device\":\"gpio\","
             "\"line\":%u,\"value\":%d}\n",
             line, value ? 1 : 0);
    bridge_send(msg);
}

static void remember_line(unsigned int *lines, unsigned int *count,
                          unsigned int line) {
    for (unsigned int i = 0; i < *count; i++) {
        if (lines[i] == line) {
            return;
        }
    }
    if (*count < MAX_TRACKED_LINES) {
        lines[*count] = line;
        (*count)++;
    }
}

static void gpio_open(fuse_req_t req, struct fuse_file_info *fi) {
    gpio_session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        fuse_reply_err(req, ENOMEM);
        return;
    }
    fi->fh = (uint64_t)(uintptr_t)s;
    fuse_reply_open(req, fi);
}

static void gpio_release(fuse_req_t req, struct fuse_file_info *fi) {
    free((void *)(uintptr_t)fi->fh);
    fuse_reply_err(req, 0);
}

static void reply_chipinfo(fuse_req_t req, void *arg, size_t out_bufsz) {
    if (out_bufsz < sizeof(struct gpiochip_info)) {
        struct iovec out_iov = {
            .iov_base = arg,
            .iov_len = sizeof(struct gpiochip_info),
        };
        fuse_reply_ioctl_retry(req, NULL, 0, &out_iov, 1);
        return;
    }

    struct gpiochip_info info;
    memset(&info, 0, sizeof(info));
    strncpy(info.name, "gpiochip0_sim", sizeof(info.name) - 1);
    strncpy(info.label, "AgentCockpit CUSE GPIO", sizeof(info.label) - 1);
    info.lines = GPIO_LINES;
    fuse_reply_ioctl(req, 0, &info, sizeof(info));
}

static void handle_lineinfo(fuse_req_t req, void *arg, unsigned flags,
                            const void *in_buf, size_t in_bufsz) {
    if (!(flags & FUSE_IOCTL_DIR) || in_bufsz < sizeof(struct gpioline_info)) {
        struct iovec in_iov = {
            .iov_base = arg,
            .iov_len = sizeof(struct gpioline_info),
        };
        fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
        return;
    }

    struct gpioline_info info;
    memcpy(&info, in_buf, sizeof(info));

    const line_def_t *def = find_line(info.line_offset);
    memset(info.name, 0, sizeof(info.name));
    memset(info.consumer, 0, sizeof(info.consumer));
    if (def) {
        strncpy(info.name, def->name, sizeof(info.name) - 1);
        info.flags = def->is_output ? GPIOLINE_FLAG_IS_OUT : 0;
    } else {
        snprintf(info.name, sizeof(info.name), "GPIO%u", info.line_offset);
        info.flags = 0;
    }

    fuse_reply_ioctl(req, 0, &info, sizeof(info));
}

static void handle_linehandle_request(fuse_req_t req, struct fuse_file_info *fi,
                                      void *arg, unsigned flags,
                                      const void *in_buf, size_t in_bufsz) {
    if (!(flags & FUSE_IOCTL_DIR) || in_bufsz < sizeof(struct gpiohandle_request)) {
        struct iovec in_iov = {
            .iov_base = arg,
            .iov_len = sizeof(struct gpiohandle_request),
        };
        fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
        return;
    }

    gpio_session_t *s = (gpio_session_t *)(uintptr_t)fi->fh;
    struct gpiohandle_request r;
    memcpy(&r, in_buf, sizeof(r));

    if (r.lines == 0 || r.lines > GPIOHANDLES_MAX) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    int is_output = (r.flags & GPIOHANDLE_REQUEST_OUTPUT) ? 1 : 0;
    for (unsigned int i = 0; i < r.lines; i++) {
        unsigned int line = r.lineoffsets[i];
        if (line >= GPIO_LINES) {
            fuse_reply_err(req, EINVAL);
            return;
        }
        register_line(line, is_output ? "out" : "in");
        if (is_output) {
            remember_line(s->output_lines, &s->output_count, line);
            set_line_value(line, r.default_values[i]);
        } else {
            remember_line(s->input_lines, &s->input_count, line);
        }
    }

    /*
     * This is intentionally not a real line fd. CUSE cannot install one into
     * the caller's fd table. Returning 0 keeps ABI probes deterministic while
     * making the limitation visible to transparent clients.
     */
    r.fd = 0;
    fuse_reply_ioctl(req, 0, &r, sizeof(r));
}

static void handle_set_values(fuse_req_t req, struct fuse_file_info *fi,
                              void *arg, unsigned flags,
                              const void *in_buf, size_t in_bufsz) {
    if (!(flags & FUSE_IOCTL_DIR) || in_bufsz < sizeof(struct gpiohandle_data)) {
        struct iovec in_iov = {
            .iov_base = arg,
            .iov_len = sizeof(struct gpiohandle_data),
        };
        fuse_reply_ioctl_retry(req, &in_iov, 1, NULL, 0);
        return;
    }

    gpio_session_t *s = (gpio_session_t *)(uintptr_t)fi->fh;
    const struct gpiohandle_data *d = in_buf;

    for (unsigned int i = 0; i < s->output_count && i < GPIOHANDLES_MAX; i++) {
        set_line_value(s->output_lines[i], d->values[i]);
    }

    fuse_reply_ioctl(req, 0, NULL, 0);
}

static void handle_get_values(fuse_req_t req, struct fuse_file_info *fi) {
    gpio_session_t *s = (gpio_session_t *)(uintptr_t)fi->fh;
    struct gpiohandle_data d;
    memset(&d, 0, sizeof(d));

    for (unsigned int i = 0; i < s->input_count && i < GPIOHANDLES_MAX; i++) {
        d.values[i] = bridge_get_button(s->input_lines[i]);
    }

    fuse_reply_ioctl(req, 0, &d, sizeof(d));
}

static void gpio_ioctl(fuse_req_t req, int cmd, void *arg,
                       struct fuse_file_info *fi, unsigned flags,
                       const void *in_buf, size_t in_bufsz, size_t out_bufsz) {
    (void)out_bufsz;

    uint32_t ucmd = (uint32_t)cmd;

    switch (ucmd) {
    case GPIO_GET_CHIPINFO_IOCTL:
        reply_chipinfo(req, arg, out_bufsz);
        break;

    case GPIO_GET_LINEINFO_IOCTL:
        handle_lineinfo(req, arg, flags, in_buf, in_bufsz);
        break;

    case GPIO_GET_LINEHANDLE_IOCTL:
        handle_linehandle_request(req, fi, arg, flags, in_buf, in_bufsz);
        break;

    case GPIOHANDLE_SET_LINE_VALUES_IOCTL:
        handle_set_values(req, fi, arg, flags, in_buf, in_bufsz);
        break;

    case GPIOHANDLE_GET_LINE_VALUES_IOCTL:
        handle_get_values(req, fi);
        break;

    default:
        fprintf(stderr, "[cuse_gpio] unknown ioctl 0x%08x\n", ucmd);
        fuse_reply_err(req, ENOTTY);
        break;
    }
}

static const struct cuse_lowlevel_ops gpio_clops = {
    .open = gpio_open,
    .release = gpio_release,
    .ioctl = gpio_ioctl,
};

int main(int argc, char *argv[]) {
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *devname = "gpiochip0";

    char **fuse_argv = malloc((argc + 1) * sizeof(char *));
    if (!fuse_argv) {
        return 1;
    }

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

    char dev_name_buf[64];
    snprintf(dev_name_buf, sizeof(dev_name_buf), "DEVNAME=%s", devname);
    const char *dev_info_argv[] = { dev_name_buf };

    struct cuse_info ci = {
        .dev_major = 0,
        .dev_minor = 0,
        .dev_info_argc = 1,
        .dev_info_argv = dev_info_argv,
        .flags = CUSE_UNRESTRICTED_IOCTL,
    };

    fprintf(stderr, "[cuse_gpio] starting /dev/%s stub\n", devname);
    int ret = cuse_lowlevel_main(args.argc, args.argv, &ci, &gpio_clops, NULL);
    free(fuse_argv);
    return ret;
}
