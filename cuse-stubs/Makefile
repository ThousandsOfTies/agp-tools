# cuse-stubs/Makefile — simulation tools build

CC = aarch64-linux-gnu-gcc

SUBDIRS = gpio-shim spi-shim i2c-stub test

.PHONY: all clean $(SUBDIRS)

all:
	$(MAKE) -C gpio-shim  CC=$(CC)
	$(MAKE) -C spi-shim   CC=$(CC)
	$(MAKE) -C i2c-stub   CC=$(CC)
	$(MAKE) -C test       CC=$(CC)

clean:
	$(MAKE) -C gpio-shim  clean
	$(MAKE) -C spi-shim   clean
	$(MAKE) -C i2c-stub   clean
	$(MAKE) -C test       clean
