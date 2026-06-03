CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64 -I/usr/include/fuse3
LDFLAGS  = -lfuse3 -lpthread

# ARM build from this subdirectory: make CC=aarch64-linux-gnu-gcc

SRCS = cuse_spi.c mfrc522_sim.c
BIN  = cuse_spi

$(BIN): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(BIN)

# Helper: run the stub (needs /dev/fuse accessible)
run: $(BIN)
	sudo ./$(BIN) -f --devname=spidev0.0

# Run without sudo if group 'fuse' is set up:
run-user: $(BIN)
	./$(BIN) -f --devname=spidev0.0

.PHONY: clean run run-user
