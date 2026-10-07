/* MCP23017 — a 16-bit I²C port expander, as the boards here use it.
 *
 * Deliberately small: direction and output latch of either port, one pin at a time or a
 * whole port at once. What it is for — outputs, LEDs, relays — belongs to the
 * application (`apps/out8`); this file knows registers, not endpoints.
 *
 * The I²C bus is the application's: it creates it and hands it in. More than one device
 * can then share the bus, and the pins of the bus are configured in one place.
 *
 * Register addresses assume IOCON.BANK = 0 — the power-on default, which nothing here
 * changes.
 */
#ifndef MCP23017_H
#define MCP23017_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MCP23017_PORT_A = 0,  /**< GPA0 … GPA7, pins 21 … 28 */
    MCP23017_PORT_B = 1,  /**< GPB0 … GPB7, pins 1 … 8 */
} mcp23017_port_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    /** The output latches as last written. Kept here rather than read back before
     * every change: a pin change is then one write, not a read and a write. Valid
     * only after the port has been written once — see `mcp23017_write_port`. */
    uint8_t olat[2];
} mcp23017_t;

/** Attach the chip at `address` (0x20 … 0x27, set by A0 … A2) to `bus`.
 *
 * Writes nothing to the chip. Its registers are whatever they were: the power-on state
 * after a power cut, the last state after a restart of the processor alone (RESET tied
 * to 3V3). Writing them in full before use is the caller's job — the order matters to it
 * (latch before direction, so a pin becomes an output already at the right level).
 */
esp_err_t mcp23017_init(mcp23017_t *m, i2c_master_bus_handle_t bus, uint8_t address,
                        uint32_t scl_speed_hz);

/** Direction of a whole port: a 1 bit is an input (the power-on state), 0 an output. */
esp_err_t mcp23017_set_direction(mcp23017_t *m, mcp23017_port_t port, uint8_t iodir);

/** The whole output latch of a port, one bit per pin; remembered for `mcp23017_write_pin`. */
esp_err_t mcp23017_write_port(mcp23017_t *m, mcp23017_port_t port, uint8_t olat);

/** One pin of the latch; the other seven keep their remembered level. */
esp_err_t mcp23017_write_pin(mcp23017_t *m, mcp23017_port_t port, uint8_t pin, bool level);

#ifdef __cplusplus
}
#endif

#endif /* MCP23017_H */
