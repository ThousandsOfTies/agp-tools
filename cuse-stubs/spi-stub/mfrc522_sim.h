#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * MFRC-522 register-level simulation (SPI).
 *
 * Ported from the LD_PRELOAD spi_shim so that the same MFRC-522 register
 * protocol can be served behind a real /dev/spidev0.0 character device
 * created via CUSE. Card-present state is queried from the web bridge
 * (GAR_HW_SIM_SOCK or GAR_RUNTIME_DIR/hw_sim.sock), so `gar sim ui rfid tap`
 * drives the simulated reader.
 */

void mfrc522_sim_init(void);

/*
 * Process one SPI transfer of `len` bytes.
 *
 * MFRC-522 SPI protocol uses 2-byte transfers:
 *   tx[0]: address byte = (reg << 1) & 0x7E, MSB=1 READ / 0 WRITE
 *   tx[1]: data (write) or 0x00 (read); response is placed in rx[1]
 *
 * `tx` may be NULL (treated as all-zero). `rx` may be NULL (response
 * discarded). Bytes beyond index 1 are zero-filled in `rx`.
 */
void mfrc522_sim_transfer(const uint8_t *tx, uint8_t *rx, size_t len);
