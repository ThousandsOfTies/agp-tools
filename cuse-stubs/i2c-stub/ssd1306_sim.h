#ifndef SSD1306_SIM_H
#define SSD1306_SIM_H

#include <stddef.h>
#include <stdint.h>

#define SSD1306_SIM_ADDR  0x3C

void ssd1306_sim_init(void);
void ssd1306_sim_write(const uint8_t *buf, size_t len);

#endif
