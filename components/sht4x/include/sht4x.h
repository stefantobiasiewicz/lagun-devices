/* SHT4x — Sensirion temperature and humidity sensor (SHT40 here), as the boards use it.
 *
 * Small, like `mcp23017`: one high-precision measurement and a soft reset. What a reading
 * means on the bus — `Sense`, offsets, thresholds — belongs to the library; this file
 * knows commands and the CRC, not endpoints. The I²C bus is the application's.
 *
 * Two calls per measurement, not one: the conversion takes up to 8.3 ms, and the node's
 * loop runs every 2 ms — a driver that slept through it would hold the bus answers back.
 * `sht4x_start`, at least `SHT4X_MEASURE_MS` later `sht4x_fetch`.
 *
 * No heater (zadania/zrobione/DEVICE-01.md N6): it evaporates condensation near 100 % RH,
 * which a room does not see, and it warms the chip, which falsifies the temperature.
 *
 * Commands, timing, CRC and the conversion follow the Sensirion datasheet and the Zephyr
 * driver `drivers/sensor/sensirion/sht4x` (Apache-2.0, the Zephyr Project contributors).
 */
#ifndef SHT4X_H
#define SHT4X_H

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHT4X_ADDRESS     0x44u
/** High precision: 8.3 ms at most by the datasheet; 10 leaves a margin. */
#define SHT4X_MEASURE_MS  10u

typedef struct {
    i2c_master_dev_handle_t dev;
} sht4x_t;

typedef struct {
    int32_t temperature; /**< 0.01 °C — the bus's unit for `TEMPERATURE` */
    int32_t humidity;    /**< 0.01 %, held to 0 … 100 % */
} sht4x_reading_t;

/** Attach the chip at `address` to `bus`. */
esp_err_t sht4x_init(sht4x_t *s, i2c_master_bus_handle_t bus, uint8_t address,
                     uint32_t scl_speed_hz);

/** Start one high-precision measurement (command 0xFD). */
esp_err_t sht4x_start(sht4x_t *s);

/** Read the measurement started `SHT4X_MEASURE_MS` ago. `ESP_ERR_INVALID_CRC` when a
 * word fails its CRC — the reading is not to be trusted, which is not the same as zero
 * (types/0x0030-sense.md §5.2). */
esp_err_t sht4x_fetch(sht4x_t *s, sht4x_reading_t *out);

/** Soft reset (0x94); the chip is ready 1 ms later. */
esp_err_t sht4x_reset(sht4x_t *s);

/** CRC-8 of the datasheet: polynomial 0x31, start 0xFF, no reflection. For tests too. */
uint8_t sht4x_crc(const uint8_t *data, int len);

#ifdef __cplusplus
}
#endif

#endif /* SHT4X_H */
