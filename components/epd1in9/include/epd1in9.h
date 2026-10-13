/* Waveshare 1.9inch Segment e-Paper (IST7134) — the panel, as `apps/epd` uses it.
 *
 * Hardware only (zadania/zrobione/DEVICE-04.md, N12): characters to segment bytes, the
 * controller's commands, the waveforms, the reset and BUSY pins. What the number in a
 * place is, and when the glass may change, are the library's tools
 * (`lagunbus/tools/digits.h`, `lagunbus/tools/epaper.h`), tested on a computer.
 *
 * Where it comes from: the maker's notes have no segment map and no command list. The
 * commands, the waveforms and the I²C addresses are those of Waveshare's demo
 * (`EPD_1in9`, E-Paper-Segment-Code.zip); the places of the digits come from a community
 * driver for the ESP32 (nanomad/pio-esp32-waveshare-1in9-demo) and agree with the drawing
 * of the panel, and the test pattern of `apps/epd` confirmed them on the glass
 * (2026-10-13: every digit in every place, the minus, the dashes, the half digit — as the
 * log said). The image, 15 bytes:
 *
 *     0          the half digit "1" of the temperature
 *     1–2, 3–4   tens and units of the temperature — the point is bit 0x20 of byte 4
 *     11–12      tenths of the temperature (the small digit)
 *     5–6, 7–8   tens and units of the humidity — the point is bit 0x20 of byte 8
 *     9–10       tenths of the humidity — `%` is bit 0x20 of byte 10
 *     13         °C 0x05, °F 0x06, Bluetooth 0x08, power 0x10
 *     14         unused
 *
 * NEVER WAITS (N10). A refresh takes 0.3–1.5 s and the node's loop turns every 2 ms, its
 * answers due within 100 ms: `epd1in9_refresh` sends the picture and returns,
 * `epd1in9_tick` watches BUSY and puts the controller to sleep once it is done. Only
 * `epd1in9_init` blocks (~550 ms of reset and power-up) — it runs before the node starts.
 *
 * The I²C bus is the application's, as for `components/mcp23017`.
 */
#ifndef EPD1IN9_H
#define EPD1IN9_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EPD1IN9_IMAGE 15u
#define EPD1IN9_ADDRESS_COMMAND 0x3Cu
#define EPD1IN9_ADDRESS_DATA    0x3Du

/** Places drawn by `epd1in9_draw`: "188.8" and "88.8", as `lb_digits_format` gives them. */
#define EPD1IN9_TEMPERATURE_PLACES 5u
#define EPD1IN9_HUMIDITY_PLACES    4u

/** The demo's three waveforms (DEVICE-04, N11). */
typedef enum {
    EPD1IN9_WAVE_BOOT,  /**< "5S" — the least ghosting; the clear at start */
    EPD1IN9_WAVE_FULL,  /**< "GC" — the whole glass flashes */
    EPD1IN9_WAVE_QUICK, /**< "DU" — the partial refresh, 0.3 s */
} epd1in9_wave_t;

typedef struct {
    i2c_master_dev_handle_t command;
    i2c_master_dev_handle_t data;
    gpio_num_t busy;
    bool       refreshing;
    bool       seen_busy;  /**< BUSY went low during this refresh */
    uint32_t   started_ms;
} epd1in9_t;

/** Pins, both I²C addresses, a hardware reset and the power-up of the controller, with the
 * frame time for a room (N11). Blocks ~550 ms. */
esp_err_t epd1in9_init(epd1in9_t *panel, i2c_master_bus_handle_t bus, gpio_num_t rst,
                       gpio_num_t busy, uint32_t scl_speed_hz);

/** The picture: `temperature` in "188.8" places with °C, `humidity` in "88.8" with %.
 * Characters as `lagunbus/tools/digits.h` gives them; one the glass cannot draw is blank.
 * NULL leaves that field blank. */
void epd1in9_draw(uint8_t image[EPD1IN9_IMAGE], const char *temperature,
                  const char *humidity);

/** Start a refresh with `wave`: wake, waveform, picture, display on. Returns at once; an
 * error means the picture did not reach the controller. Not while `epd1in9_busy`. */
esp_err_t epd1in9_refresh(epd1in9_t *panel, const uint8_t image[EPD1IN9_IMAGE],
                          epd1in9_wave_t wave, uint32_t now_ms);

/** Every turn of the loop: once BUSY says the refresh is over, the controller sleeps. */
void epd1in9_tick(epd1in9_t *panel, uint32_t now_ms);

/** A refresh is under way — nothing may be sent. */
bool epd1in9_busy(const epd1in9_t *panel);

#ifdef __cplusplus
}
#endif

#endif /* EPD1IN9_H */
