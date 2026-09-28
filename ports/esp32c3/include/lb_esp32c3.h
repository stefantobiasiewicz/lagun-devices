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

#include "lagunbus/platform.h"
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
