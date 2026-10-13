/* VEML7700 — Vishay ambient light sensor, as the boards use it.
 *
 * The sensor measures continuously; a read takes the last finished conversion. What
 * this driver adds is the RANGE (zadania/zrobione/DEVICE-01.md N4): a fixed gain and
 * integration time either saturates in sunlight or sees nothing at dusk, so after every
 * read it steps one rung up or down a ladder of settings — from gain ×2 / 800 ms
 * (0.0036 lx a count) to gain ×1/8 / 25 ms (1.84 lx a count, ~120 klx full scale) — to
 * keep the count between 100 and 10 000, as Vishay's application note "Designing the
 * VEML7700 Into an Application" recommends. A read right after a change belongs to the
 * old setting half-way and is thrown away (`VEML7700_SETTLING`).
 *
 * Above ~1000 lx the response is not linear; the application note's polynomial corrects
 * it. Floats here, where it is cheap at one read a second; the bus gets an integer.
 *
 * Registers, the resolution table and the correction follow the Vishay datasheet and
 * application note and the Zephyr driver `drivers/sensor/vishay/veml7700` (Apache-2.0,
 * the Zephyr Project contributors).
 */
#ifndef VEML7700_H
#define VEML7700_H

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VEML7700_ADDRESS 0x10u

/** `veml7700_read` after a change of range: the conversion in progress is half old. */
#define VEML7700_SETTLING ESP_ERR_NOT_FINISHED

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t rung;      /**< index into the ladder of (gain, integration time) */
    uint8_t settling;  /**< reads to throw away after a change of range */
} veml7700_t;

/** Attach the chip at `address` to `bus`. Configure it with `veml7700_configure`. */
esp_err_t veml7700_init(veml7700_t *v, i2c_master_bus_handle_t bus, uint8_t address,
                        uint32_t scl_speed_hz);

/** Power on at the middle of the ladder — also what a "reset" is for this chip, which
 * has no reset command (N7). Waits the 2.5 ms the datasheet asks after power-on. */
esp_err_t veml7700_configure(veml7700_t *v);

/** The illuminance in 0.01 lx — the bus's unit for `ILLUMINANCE` — and a step of the
 * range for the next read when the count left 100 … 10 000. `VEML7700_SETTLING` after a
 * change of range; a saturated count is a change of range too, never a value. */
esp_err_t veml7700_read(veml7700_t *v, uint32_t *centilux);

/** The integration time of the current range, in ms — reading more often gains nothing. */
uint32_t veml7700_integration_ms(const veml7700_t *v);

#ifdef __cplusplus
}
#endif

#endif /* VEML7700_H */
