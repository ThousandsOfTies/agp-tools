CC     = gcc
CFLAGS = -Wall -O2 -fPIC
LDFLAGS = -shared -ldl -lpthread

# Cross-compile: make CC=aarch64-linux-gnu-gcc
TARGET = spi_shim.so

$(TARGET): spi_shim.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
