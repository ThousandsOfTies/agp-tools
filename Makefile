# Gapless Agent Runtime simulation tools

CC = aarch64-linux-gnu-gcc

.PHONY: all clean

all:
	$(MAKE) -C cuse-stubs CC=$(CC)

clean:
	$(MAKE) -C cuse-stubs clean
