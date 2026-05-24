/*
 * vl53l0x_read.c — VL53L0X distance readout via /dev/i2c-1
 *
 * Same binary runs on:
 *   - EC2 arm64 (with CUSE stub providing /dev/i2c-1)
 *   - RasPi5    (with physical VL53L0X wired on I2C bus 1)
 *
 * Uses I2C_SLAVE + write()/read() instead of I2C_RDWR.
 * Both are standard i2c-dev APIs and work identically on real hardware.
 *
 * Usage:  ./vl53l0x_read [/dev/i2c-1]
 */

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#define VL53L0X_ADDR            0x29
#define REG_MODEL_ID            0xC0
#define REG_REVISION_ID         0xC2
#define REG_SYSRANGE_START      0x00
#define REG_RESULT_INT_STATUS   0x13
#define REG_RESULT_RANGE_STATUS 0x14
#define REG_RANGE_HIGH          0x1E
#define REG_RANGE_LOW           0x1F
#define REG_INT_CLEAR           0x0B

static int fd;

static int write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return write(fd, buf, 2) == 2 ? 0 : -1;
}

static uint8_t read_reg(uint8_t reg) {
    uint8_t val = 0;
    if (write(fd, &reg, 1) != 1) return 0;
    if (read(fd, &val, 1) != 1) return 0;
    return val;
}

static int poll_interrupt(int max_tries) {
    for (int i = 0; i < max_tries; i++) {
        if (read_reg(REG_RESULT_INT_STATUS) & 0x07) return 1;
        usleep(10000);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *devpath = (argc > 1) ? argv[1] : "/dev/i2c-1";

    fd = open(devpath, O_RDWR);
    if (fd < 0) { perror(devpath); return 1; }

    if (ioctl(fd, I2C_SLAVE, VL53L0X_ADDR) < 0) {
        perror("I2C_SLAVE"); close(fd); return 1;
    }

    uint8_t model_id = read_reg(REG_MODEL_ID);
    uint8_t rev_id   = read_reg(REG_REVISION_ID);
    printf("Model ID: 0x%02X  Revision: 0x%02X\n", model_id, rev_id);

    if (model_id != 0xEE) {
        fprintf(stderr, "Unexpected Model ID (expected 0xEE)\n");
        close(fd); return 1;
    }

    printf("VL53L0X detected. Taking 5 measurements...\n\n");

    for (int n = 0; n < 5; n++) {
        write_reg(REG_SYSRANGE_START, 0x01);

        if (!poll_interrupt(50)) {
            fprintf(stderr, "  [%d] Timeout\n", n);
            continue;
        }

        uint8_t hi     = read_reg(REG_RANGE_HIGH);
        uint8_t lo     = read_reg(REG_RANGE_LOW);
        uint8_t status = read_reg(REG_RESULT_RANGE_STATUS);
        uint16_t range_mm = ((uint16_t)hi << 8) | lo;

        printf("  [%d] Range: %4u mm  (status=0x%02X)\n", n, range_mm, status);

        write_reg(REG_INT_CLEAR, 0x01);
        usleep(50000);
    }

    close(fd);
    return 0;
}
