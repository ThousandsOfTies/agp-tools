# cuse-stubs/Makefile — トップレベルビルド
#
# クロスコンパイル (Codespaces x86_64 → EC2 arm64):
#   make cross
#
# ネイティブビルド (EC2 / RasPi5 上で直接):
#   make native
#
# EC2 に転送 (SSH Host名 または IP アドレスを指定):
#   make deploy EC2=vibecode-graviton
#   make deploy EC2=ubuntu@1.2.3.4 KEY=~/.ssh/vibecode-graviton.pem

CC_NATIVE = gcc
CC_CROSS  = aarch64-linux-gnu-gcc

SUBDIRS = gpio-shim spi-shim i2c-stub test

# SSH key: ~/.ssh/config のエントリがあれば不要、IP直指定時に使う
KEY     ?=
SCP     = scp $(if $(KEY),-i $(KEY),)
SSH_DST = $(if $(KEY),ubuntu@$(EC2),$(EC2))

.PHONY: cross native deploy clean $(SUBDIRS)

cross:
	$(MAKE) -C gpio-shim  CC=$(CC_CROSS)
	$(MAKE) -C spi-shim   CC=$(CC_CROSS)
	$(MAKE) -C i2c-stub   CC=$(CC_CROSS)
	$(MAKE) -C test       CC=$(CC_CROSS)

native:
	$(MAKE) -C gpio-shim  CC=$(CC_NATIVE)
	$(MAKE) -C spi-shim   CC=$(CC_NATIVE)
	$(MAKE) -C i2c-stub   CC=$(CC_NATIVE)
	$(MAKE) -C test       CC=$(CC_NATIVE)

clean:
	$(MAKE) -C gpio-shim  clean
	$(MAKE) -C spi-shim   clean
	$(MAKE) -C i2c-stub   clean
	$(MAKE) -C test       clean

# EC2=<ssh_host_or_ip> [KEY=<pem_path>] を指定して実行
deploy:
ifndef EC2
	$(error EC2 変数を指定してください: make deploy EC2=vibecode-graviton  または  make deploy EC2=1.2.3.4 KEY=~/.ssh/vibecode-graviton.pem)
endif
	$(SCP) gpio-shim/gpio_shim.so   $(SSH_DST):~/
	$(SCP) spi-shim/spi_shim.so     $(SSH_DST):~/
	$(SCP) test/gpio_led_button     $(SSH_DST):~/
	$(SCP) test/vl53l0x_read        $(SSH_DST):~/
	$(SCP) i2c-stub/cuse_i2c        $(SSH_DST):~/
	$(SCP) -r web-bridge            $(SSH_DST):~/
	@echo "Deploy complete → ssh $(SSH_DST)"
