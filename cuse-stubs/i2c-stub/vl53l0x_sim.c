#include "vl53l0x_sim.h"
#include <string.h>
#include <stdio.h>

/* 256-byte register file */
static uint8_t regs[256];
static uint16_t simulated_range_mm = 300; /* default 300mm */
static int measurement_pending = 0;

void vl53l0x_sim_init(void) {
    memset(regs, 0, sizeof(regs));

    /* VL53L0X identification values (from datasheet) */
    regs[REG_IDENTIFICATION_MODEL_ID]    = 0xEE;
    regs[REG_IDENTIFICATION_REVISION_ID] = 0x10;
    regs[REG_VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV] = 0x00;

    /* No interrupt pending initially */
    regs[REG_RESULT_INTERRUPT_STATUS] = 0x00;

    /* Range status: no error, not ready */
    regs[REG_RESULT_RANGE_STATUS] = 0x00;

    fprintf(stderr, "[vl53l0x_sim] initialized, range=%umm\n", simulated_range_mm);
}

void vl53l0x_sim_set_range_mm(uint16_t mm) {
    simulated_range_mm = mm;
}

static void update_range_result(void) {
    /*
     * RESULT_RANGE_STATUS (0x14) layout per datasheet:
     *   [7:3] = range_status (0x0 = no error)
     *   [0]   = DeviceReadyForNewMeasurement
     *
     * Range value is at 0x1E (high byte) and 0x1F (low byte).
     */
    regs[REG_RESULT_RANGE_STATUS] = (1 << 0); /* ready */
    regs[0x1E] = (simulated_range_mm >> 8) & 0xFF;
    regs[0x1F] = simulated_range_mm & 0xFF;
    /* Interrupt bit: data ready */
    regs[REG_RESULT_INTERRUPT_STATUS] = 0x07;
}

uint8_t vl53l0x_sim_read_reg(uint8_t reg) {
    if (measurement_pending) {
        update_range_result();
        measurement_pending = 0;
    }
    return regs[reg];
}

void vl53l0x_sim_write_reg(uint8_t reg, uint8_t val) {
    regs[reg] = val;

    if (reg == REG_SYSRANGE_START && (val & 0x01)) {
        /* Single shot measurement triggered */
        measurement_pending = 1;
        fprintf(stderr, "[vl53l0x_sim] measurement triggered, will return %umm\n",
                simulated_range_mm);
    }

    if (reg == REG_SYSTEM_INTERRUPT_CLEAR) {
        regs[REG_RESULT_INTERRUPT_STATUS] = 0x00;
    }
}
