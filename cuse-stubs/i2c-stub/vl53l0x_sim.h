#pragma once
#include <stdint.h>

/* VL53L0X I2C address (7-bit) */
#define VL53L0X_ADDR 0x29

/* Key registers */
#define REG_IDENTIFICATION_MODEL_ID         0xC0
#define REG_IDENTIFICATION_REVISION_ID      0xC2
#define REG_VHV_CONFIG_PAD_SCL_SDA_EXTSUP_HV 0x89
#define REG_RESULT_RANGE_STATUS             0x14
#define REG_RESULT_INTERRUPT_STATUS         0x13
#define REG_SYSRANGE_START                  0x00
#define REG_SYSTEM_INTERRUPT_CLEAR          0x0B

void vl53l0x_sim_init(void);
uint8_t vl53l0x_sim_read_reg(uint8_t reg);
void vl53l0x_sim_write_reg(uint8_t reg, uint8_t val);

/* Simulated measurement (mm) — writable for test injection */
void vl53l0x_sim_set_range_mm(uint16_t mm);
