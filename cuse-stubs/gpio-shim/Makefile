CC     = gcc
CFLAGS = -Wall -O2 -fPIC
LDFLAGS = -shared -ldl -lpthread

# ARM build from this subdirectory: make CC=aarch64-linux-gnu-gcc
TARGET = gpio_shim.so

$(TARGET): gpio_shim.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
