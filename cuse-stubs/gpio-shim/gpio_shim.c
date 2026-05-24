/*
 * gpio_shim.c — LD_PRELOAD shim for /dev/gpiochip0
 *
 * Intercepts gpiochip ioctl calls and routes state to the web bridge
 * via a Unix domain socket (/tmp/hw_sim.sock).
 *
 * EC2:    LD_PRELOAD=./gpio_shim.so ./gpio_led_button
 * RasPi5: ./gpio_led_button   (no preload, uses real /dev/gpiochip0)
 *
 * LED  → bridge → HTML (glowing circle)
 * BTN  ← bridge ← HTML (button click)
 */

#define _GNU_SOURCE
#include <sys/mman.h>   /* memfd_create */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SIM_SOCK  "/tmp/hw_sim.sock"
#define MAX_FDS   1024

/* ------------------------------------------------------------------ */
/* fd registries                                                        */
/* ------------------------------------------------------------------ */

static int chip_fd_flag[MAX_FDS];  /* 1 if fd is a fake gpiochip fd */

typedef struct {
    int   active;
    int   lines[GPIOHANDLES_MAX];
    int   nlines;
    int   is_output;  /* 1=OUTPUT 0=INPUT */
} handle_t;

static handle_t handles[MAX_FDS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Bridge socket (persistent connection)                                */
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

static void bridge_send(const char *json_line) {
    int s = bridge_connect();
    if (s < 0) {
        fprintf(stderr, "[gpio_shim] bridge not available: %s\n", json_line);
        return;
    }
    if (write(s, json_line, strlen(json_line)) < 0)
        fprintf(stderr, "[gpio_shim] bridge_send failed\n");
}

/* Query button state synchronously (request-response) */
static int bridge_get_button(int line) {
    int s = bridge_connect();
    if (s < 0) return 0;

    char req[128];
    snprintf(req, sizeof(req),
             "{\"req\":\"get\",\"device\":\"gpio\",\"line\":%d}\n", line);
    if (write(s, req, strlen(req)) < 0) return 0;

    char resp[128] = {0};
    int n = read(s, resp, sizeof(resp) - 1);
    if (n <= 0) return 0;

    /* Parse {"value":N} */
    char *p = strstr(resp, "\"value\":");
    return p ? atoi(p + 8) : 0;
}

/* ------------------------------------------------------------------ */
/* Intercepted libc functions                                           */
/* ------------------------------------------------------------------ */

static int  (*real_open)(const char *, int, ...)   = NULL;
static int  (*real_open64)(const char *, int, ...) = NULL;
static int  (*real_ioctl)(int, unsigned long, ...) = NULL;
static int  (*real_close)(int)                     = NULL;

__attribute__((constructor)) static void shim_init(void) {
    real_open   = dlsym(RTLD_NEXT, "open");
    real_open64 = dlsym(RTLD_NEXT, "open64");
    real_ioctl  = dlsym(RTLD_NEXT, "ioctl");
    real_close  = dlsym(RTLD_NEXT, "close");
    fprintf(stderr, "[gpio_shim] loaded, bridge=%s\n", SIM_SOCK);
}

/* open: intercept /dev/gpiochip* */
int open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);

    if (strncmp(path, "/dev/gpiochip", 13) == 0) {
        int fd = memfd_create("gpiochip_sim", 0);
        if (fd >= 0 && fd < MAX_FDS) {
            pthread_mutex_lock(&mu);
            chip_fd_flag[fd] = 1;
            pthread_mutex_unlock(&mu);
            fprintf(stderr, "[gpio_shim] open(%s) → fd=%d\n", path, fd);
        }
        return fd;
    }
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    mode_t mode = va_arg(ap, mode_t); va_end(ap);

    if (strncmp(path, "/dev/gpiochip", 13) == 0) {
        int fd = memfd_create("gpiochip_sim", 0);
        if (fd >= 0 && fd < MAX_FDS) {
            pthread_mutex_lock(&mu);
            chip_fd_flag[fd] = 1;
            pthread_mutex_unlock(&mu);
        }
        return fd;
    }
    return real_open64(path, flags, mode);
}

/* close: clean up handle state */
int close(int fd) {
    if (fd >= 0 && fd < MAX_FDS) {
        pthread_mutex_lock(&mu);
        chip_fd_flag[fd] = 0;
        handles[fd].active = 0;
        pthread_mutex_unlock(&mu);
    }
    return real_close(fd);
}

/* ioctl: main interception logic */
int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *); va_end(ap);

    if (fd < 0 || fd >= MAX_FDS) return real_ioctl(fd, req, arg);

    pthread_mutex_lock(&mu);
    int is_chip   = chip_fd_flag[fd];
    int is_handle = handles[fd].active;
    pthread_mutex_unlock(&mu);

    /* ---- gpiochip fd ioctls ---- */
    if (is_chip) {
        if (req == GPIO_GET_CHIPINFO_IOCTL) {
            struct gpiochip_info *info = arg;
            memset(info, 0, sizeof(*info));
            strncpy(info->name,  "gpiochip0_sim", sizeof(info->name) - 1);
            strncpy(info->label, "sim",           sizeof(info->label) - 1);
            info->lines = 54;
            return 0;
        }

        if (req == GPIO_GET_LINEINFO_IOCTL) {
            struct gpioline_info *info = arg;
            snprintf(info->name, sizeof(info->name), "GPIO%u", info->line_offset);
            info->flags = 0;
            return 0;
        }

        if (req == GPIO_GET_LINEHANDLE_IOCTL) {
            struct gpiohandle_request *r = arg;

            /* Create a socketpair; give one end to caller as handle fd */
            int sv[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;

            /* Store handle metadata on sv[0] */
            int hfd = sv[0];
            close(sv[1]); /* we don't need the other end */

            pthread_mutex_lock(&mu);
            handles[hfd].active   = 1;
            handles[hfd].nlines   = r->lines;
            handles[hfd].is_output = (r->flags & GPIOHANDLE_REQUEST_OUTPUT) ? 1 : 0;
            for (int i = 0; i < r->lines; i++)
                handles[hfd].lines[i] = r->lineoffsets[i];
            pthread_mutex_unlock(&mu);

            r->fd = hfd;
            fprintf(stderr, "[gpio_shim] LINEHANDLE lines=%d output=%d hfd=%d\n",
                    r->lines, handles[hfd].is_output, hfd);

            /* Register lines with bridge */
            char msg[256];
            for (int i = 0; i < r->lines; i++) {
                snprintf(msg, sizeof(msg),
                    "{\"event\":\"register\",\"device\":\"gpio\","
                    "\"line\":%d,\"dir\":\"%s\"}\n",
                    r->lineoffsets[i],
                    handles[hfd].is_output ? "out" : "in");
                bridge_send(msg);
            }
            return 0;
        }
        return real_ioctl(fd, req, arg);
    }

    /* ---- line handle fd ioctls ---- */
    if (is_handle) {
        if (req == GPIOHANDLE_SET_LINE_VALUES_IOCTL) {
            struct gpiohandle_data *d = arg;
            for (int i = 0; i < handles[fd].nlines; i++) {
                int line = handles[fd].lines[i];
                int val  = d->values[i];
                char msg[128];
                snprintf(msg, sizeof(msg),
                    "{\"event\":\"set\",\"device\":\"gpio\","
                    "\"line\":%d,\"value\":%d}\n", line, val);
                bridge_send(msg);
                fprintf(stderr, "[gpio_shim] LED line=%d value=%d\n", line, val);
            }
            return 0;
        }

        if (req == GPIOHANDLE_GET_LINE_VALUES_IOCTL) {
            struct gpiohandle_data *d = arg;
            for (int i = 0; i < handles[fd].nlines; i++) {
                int line = handles[fd].lines[i];
                d->values[i] = bridge_get_button(line);
            }
            return 0;
        }
        return real_ioctl(fd, req, arg);
    }

    return real_ioctl(fd, req, arg);
}
