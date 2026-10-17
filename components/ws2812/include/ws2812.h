/* WS2812 — a chain of addressable RGB LEDs on one data pin, as `apps/rgb8` uses it.
 *
 * Deliberately small: bytes out on the wire, nothing else. The order of the bytes (GRB for
 * a WS2812B), the brightness curve and the current cap are the application's — this file
 * knows the timing of the wire, not colours.
 *
 * Driven by the RMT peripheral with the stock bytes encoder: every bit is one RMT symbol,
 * a "0" 0.3 µs high and 0.9 µs low, a "1" 0.9 µs high and 0.3 µs low, at a 10 MHz tick.
 * Our own small component and not `espressif/led_strip` from the component registry: the
 * container would have to fetch it from the network at build time, and the timing is the
 * whole of what it would add (zadania/zrobione/DEVICE-03.md, H2).
 *
 * The latch ("reset") is the line held low for more than 50 µs — 280 µs on newer WS2812B.
 * `ws2812_show` waits for the frame to leave and then holds the line low for 300 µs, so
 * back-to-back calls are always latched; at 10 ms between frames that costs nothing.
 */
#ifndef WS2812_H
#define WS2812_H

#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_tx.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
} ws2812_t;

/** Set up an RMT channel on `gpio`. The pin idles low — the LEDs keep what they show. */
esp_err_t ws2812_init(ws2812_t *strip, int gpio);

/** Send `len` bytes — three a LED, in the order the LEDs want them — and latch them.
 * Blocks until the frame is out: 24 bits × 1.25 µs a LED, 240 µs for eight, plus the
 * latch. */
esp_err_t ws2812_show(ws2812_t *strip, const uint8_t *bytes, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WS2812_H */
