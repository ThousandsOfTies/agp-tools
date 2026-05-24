/*
 * gpio_led_button.c — LED blink + button read via /dev/gpiochip0
 *
 * EC2:    LD_PRELOAD=../gpio-shim/gpio_shim.so ./gpio_led_button
 * RasPi5: ./gpio_led_button
 *
 * Wiring (RasPi5):
 *   LED    → GPIO18 (pin 12)  + 330Ω resistor + GND
 *   Button → GPIO17 (pin 11)  + 3.3V, pulled down
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHIP     "/dev/gpiochip0"
#define LED_LINE  18
#define BTN_LINE  17

static volatile int running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static int open_chip(void) {
    int fd = open(CHIP, O_RDWR);
    if (fd < 0) { perror(CHIP); return -1; }
    return fd;
}

static int request_output(int chip, int line, int initial) {
    struct gpiohandle_request r = {
        .lineoffsets[0]    = line,
        .flags             = GPIOHANDLE_REQUEST_OUTPUT,
        .default_values[0] = initial,
        .lines             = 1,
    };
    strncpy(r.consumer_label, "led", sizeof(r.consumer_label) - 1);
    if (ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &r) < 0) {
        perror("GPIO_GET_LINEHANDLE_IOCTL (output)");
        return -1;
    }
    return r.fd;
}

static int request_input(int chip, int line) {
    struct gpiohandle_request r = {
        .lineoffsets[0] = line,
        .flags          = GPIOHANDLE_REQUEST_INPUT,
        .lines          = 1,
    };
    strncpy(r.consumer_label, "button", sizeof(r.consumer_label) - 1);
    if (ioctl(chip, GPIO_GET_LINEHANDLE_IOCTL, &r) < 0) {
        perror("GPIO_GET_LINEHANDLE_IOCTL (input)");
        return -1;
    }
    return r.fd;
}

static void set_led(int hfd, int val) {
    struct gpiohandle_data d = { .values[0] = val };
    ioctl(hfd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &d);
}

static int get_button(int hfd) {
    struct gpiohandle_data d = {0};
    ioctl(hfd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &d);
    return d.values[0];
}

int main(void) {
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int chip = open_chip();
    if (chip < 0) return 1;

    int led_fd = request_output(chip, LED_LINE, 0);
    int btn_fd = request_input(chip, BTN_LINE);
    if (led_fd < 0 || btn_fd < 0) { close(chip); return 1; }

    printf("GPIO LED+Button demo. Press Ctrl+C to quit.\n");
    printf("  LED  → GPIO%d\n  BTN  → GPIO%d\n\n", LED_LINE, BTN_LINE);

    int led_state  = 0;
    int prev_btn   = 0;
    int blink_tick = 0;

    while (running) {
        int btn = get_button(btn_fd);

        /* Button press toggles LED */
        if (btn && !prev_btn) {
            led_state ^= 1;
            set_led(led_fd, led_state);
            printf("Button pressed → LED %s\n", led_state ? "ON" : "OFF");
        }
        prev_btn = btn;

        /* Auto-blink when LED is off */
        if (!led_state && ++blink_tick >= 10) {
            blink_tick = 0;
            static int blink = 0;
            blink ^= 1;
            set_led(led_fd, blink);
        }

        usleep(100000); /* 100ms */
    }

    set_led(led_fd, 0);
    close(led_fd);
    close(btn_fd);
    close(chip);
    printf("\nDone.\n");
    return 0;
}
