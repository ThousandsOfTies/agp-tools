CC     = gcc
CFLAGS = -Wall -O2
BINS   = vl53l0x_read gpio_led_button

all: $(BINS)

vl53l0x_read: vl53l0x_read.c
	$(CC) $(CFLAGS) -o $@ $^

gpio_led_button: gpio_led_button.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(BINS)

.PHONY: all clean
