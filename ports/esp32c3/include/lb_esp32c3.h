/* Port ESP32-C3 — TWAI, NVS, `esp_timer`.
 *
 * The three things the library cannot do on its own: the bus, time and persistent
 * storage. Everything below is **specific to ESP-IDF** and compiles nowhere
 * else — which is why it sits here and not in `components/lagunbus/`.
 *
 * TWAI is the name Espressif gave its CAN 2.0B controller. It is CAN despite the
 * name; the difference is in the trademark, not in the protocol.
 */
#ifndef LB_ESP32C3_H
#define LB_ESP32C3_H

#include "lagunbus/node.h"
#include "lagunbus/platform.h"
#include "lagunbus/tools/debounce.h"
#include "lagunbus/tools/led.h"
#include "lagunbus/tools/service.h"
#include "lagunbus/transport.h"

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t tx;
    gpio_num_t rx;
    uint32_t   bitrate;  /**< 500000 — must match the hub and every other node */
} lb_esp_bus_cfg_t;

/** Bring TWAI up and fill in `out`.
 *
 * The filter is **open**: we accept everything and the library settles the recipient.
 * Hardware filtering would be a second description of who this node is — and the
 * address changes while joining, so the filter would have to be reset on the fly.
 */
esp_err_t lb_esp_bus_init(lb_transport_t *out, const lb_esp_bus_cfg_t *cfg);

/** Check the bus state and bring the node back after `bus-off`.
 *
 * Call it from the node loop. `bus-off` is not a failure one has to restart from:
 * the controller drops there after a run of errors — when somebody unplugs the cable,
 * say — and comes back on its own once the bus works again. Restarting the device
 * would switch off the relay along the way, which for a hallway light is worse than
 * a moment without communication.
 */
void lb_esp_bus_tick(lb_transport_t *bus);

/** The service button and the status LED every device has (zadania/zrobione/CR-05.md,
 * types/0x0001-device.md §5.4).
 *
 * The same in every application, so it lives here once: the button debounced and fed to
 * `lb_service_t` (held 5 s, reset on the release, armed once seen released), the LED
 * driven by `lb_led_t` from the node's state. What is the library's (the patterns, the
 * hold rule) is tested on a computer; this is only the pins.
 */
typedef struct {
    lb_node_t     *node;
    gpio_num_t     button_gpio;
    gpio_num_t     led_gpio;
    bool           led_active_low;
    lb_debounce_t  debounce;
    lb_service_t   service;
    lb_led_t       led;
} lb_esp_status_t;

/** Set up both pins and become the node's `on_state`. Call BEFORE `lb_node_start`, so
 * the first state report reaches the LED.
 *
 * On the Super Mini: the BOOT button on GPIO9 (to ground) and the on-board LED on GPIO8
 * (lights LOW). Both are strapping pins and both are on the header — an external button
 * goes from GPIO9 to GND next to BOOT, an external LED from 3V3 through a resistor to
 * GPIO8, lit by the same LOW level (devices/README.md). */
esp_err_t lb_esp_status_init(lb_esp_status_t *s, lb_node_t *node, gpio_num_t button_gpio,
                             gpio_num_t led_gpio, bool led_active_low);

/** Every turn of the loop: read the button, act on it, blink the LED. */
void lb_esp_status_tick(lb_esp_status_t *s, uint32_t now_ms);

/** Logging that never makes the node loop wait (zadania/BUG-02.md): `ESP_LOGx` puts the
 * line in a ring buffer and a task of its own writes it to USB. Call it FIRST in
 * `app_main` — everything logged before goes the old, waiting way. */
esp_err_t lb_esp_log_init(void);

/** `esp_timer` clock, persistence in NVS, randomness from `esp_random()`.
 *
 * `esp_random()` is hardware-backed and needs no seeding — unlike `rand()` on POSIX,
 * where a seed has to be supplied, or every node draws
 * the same.
 */
esp_err_t lb_esp_platform_init(lb_platform_t *out);

/** `Uid` from eFuse — eight bytes, unique from the factory.
 *
 * The order is not arbitrary. An Espressif MAC address starts with three OUI bytes,
 * **identical across all boards**. `ShortId` used to be the four HIGH bytes of `Uid`,
 * so had `Uid` been the MAC directly, all our nodes would have had nearly the same
 * `ShortId` and collided while joining. That is why the MAC goes in reversed: the
 * unique tail ends up at the front. (`ShortId` is CRC-32 today, so the reason is gone
 * — see the comment at the implementation for why the reversal stays.)
 */
esp_err_t lb_esp_uid(uint8_t uid[8]);

#ifdef __cplusplus
}
#endif

#endif /* LB_ESP32C3_H */
