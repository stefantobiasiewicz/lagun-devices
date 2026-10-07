/* Key3 — ESP32-C3 Super Mini + CAN transceiver. Three keys: click, raw, hold.
 *
 * `apps/key` three times over. This file makes the DEVICE's decisions and nothing else;
 * the library gives the tools (lib/lagunbus-node, `key.h` and `debounce.h`, tested on a
 * computer — this device's copy in `c/sim/devices.c` against `vectors/scenarios/key3.json`).
 * The decisions, all in one place below (zadania/zrobione/FEAT-09.md):
 *
 *   - features: RAW and HOLD, NO multi-click. `EventMode` then switches between CLICK and
 *     RAW only — a write of MULTICLICK is out of range, and `lb_key_check` says so;
 *   - exposed to the hub, per key: `EventMode`, hold on/off with its threshold and tick
 *     interval, and `PressCount`;
 *   - polarity: each key shorts to ground, so a LOW pin is a press;
 *   - bounce: 15 ms, filtered here per key — not an attribute
 *     (types/0x0010-key-input.md §5.7);
 *   - `DeviceType` 0x0011 — a model of its own, not `apps/key`'s 0x0010
 *     (devices/README.md § Models).
 *
 * Each key is its own endpoint with its own mode: one can go RAW to a gate, the other two
 * stay CLICK for lights.
 */
#include "lagunbus/tools/debounce.h"
#include "lagunbus/types/key.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "key3";

#define KEY_COUNT 3

/** 15 ms is past a mechanical key's bounce and well under the ~80 ms a finger stays
 * on it. The pins are polled every 2 ms below, so a press is never missed. */
#define DEBOUNCE_MS 15u

static const gpio_num_t key_gpio[KEY_COUNT] = {
    CONFIG_LAGUN_KEY1_GPIO,
    CONFIG_LAGUN_KEY2_GPIO,
    CONFIG_LAGUN_KEY3_GPIO,
};

/* What the hub may see and tune — one array per key, because an attribute's value lives
 * in the array and every key keeps its own. The initial values are the type's
 * defaults; the hub writes its own while reconciling, but the node has to work on its
 * own too. */
#define KEY_ATTRIBUTES                       \
    {                                        \
        LB_KEY_ATTR_PRESS_COUNT(),           \
        LB_KEY_ATTR_EVENT_MODE(),            \
        LB_KEY_ATTR_HOLD_ENABLED(),          \
        LB_KEY_ATTR_HOLD_THRESHOLD(500),     \
        LB_KEY_ATTR_HOLD_TICK_INTERVAL(200), \
    }
static lb_attr_t attributes[KEY_COUNT][5] = {KEY_ATTRIBUTES, KEY_ATTRIBUTES, KEY_ATTRIBUTES};

#define KEY_ENDPOINT(i)                                               \
    {                                                                 \
        .number = (uint8_t)(4 + (i)),                                 \
        .type_id = LB_TYPE_KEYINPUT,                                  \
        .revision = 1,                                                \
        .features = LB_KEY_F_RAW | LB_KEY_F_HOLD,                     \
        .channel = (uint8_t)(1 + (i)),                                \
        .attrs = attributes[i],                                       \
        .attr_count = sizeof attributes[i] / sizeof attributes[i][0], \
        .on_check = lb_key_check,                                     \
        .feature_of = lb_key_feature_of,                              \
    }
static lb_endpoint_t endpoints[KEY_COUNT] = {KEY_ENDPOINT(0), KEY_ENDPOINT(1), KEY_ENDPOINT(2)};

/* The rest of the keys' timings. The hold ones are overridden by the attributes above;
 * the multi-click ones are never used — there is no MULTICLICK. */
static const lb_key_config_t key_config = LB_KEY_CONFIG_DEFAULT;

/** The finger is on key `i`. The key shorts to ground: a LOW pin is a press. */
static bool key_pressed(int i)
{
    return gpio_get_level(key_gpio[i]) == 0;
}


/** Print a frame with its meaning, not just its bytes — as in `apps/key`, where the
 * reason is written out. */
static void on_frame_seen(void *ctx, const lb_frame_t *frame, bool outgoing)
{
    (void)ctx;
    const char *arrow = outgoing ? "->" : "<-";

    if (lb_is_join(frame->can_id)) {
        bool announce_frame = (frame->can_id >> LB_JOIN_DIRECTION_SHIFT) & 1u;
        ESP_LOGI(TAG, "%s JOIN %s ShortId 0x%06lX", arrow,
                 announce_frame ? "ZGLOSZENIE" : "PRZYDZIAL",
                 (unsigned long)lb_join_short_id(frame->can_id));
        return;
    }

    lb_fields_t f = lb_decode_id(frame->can_id);
    char bytes_are[24] = {0};
    for (uint8_t i = 0; i < frame->len && i * 2 + 2 < sizeof bytes_are; i++) {
        snprintf(bytes_are + i * 2, 3, "%02X", frame->data[i]);
    }
    ESP_LOGI(TAG, "%s %-10s 0x%03X ep%u el 0x%02X %s", arrow,
             lb_kind_name(f.kind), f.address, f.endpoint, f.element, bytes_are);
}

static lb_transport_t bus;
static lb_platform_t  platform_impl;
static lb_node_t      node;
static lb_esp_status_t status;

/* A Kconfig `bool` set to "no" leaves its macro undefined, so it cannot be passed on as
 * it is. */
#ifdef CONFIG_LAGUN_STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW true
#else
#define STATUS_LED_ACTIVE_LOW false
#endif
static lb_key_t       key_sm[KEY_COUNT];
static lb_debounce_t  debounce[KEY_COUNT];

void app_main(void)
{
    ESP_ERROR_CHECK(lb_esp_log_init());
    ESP_ERROR_CHECK(lb_esp_platform_init(&platform_impl));

    lb_identity_t identity_cfg = {
        .node_class = LB_CLASS_CONTROL,  /* it controls something -> block 0x001..0x0FF */
        .device_type = 0x0011,           /* key3 — devices/README.md § Models */
        .vendor_id = 0x0000,
        .firmware = 0x00000100,          /* 0.1.0 */
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

    uint64_t pins = 0;
    for (int i = 0; i < KEY_COUNT; i++) {
        pins |= 1ULL << key_gpio[i];
    }
    gpio_config_t keys = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&keys));

    lb_esp_bus_cfg_t cfg = {
        .tx = CONFIG_LAGUN_TWAI_TX,
        .rx = CONFIG_LAGUN_TWAI_RX,
        .bitrate = CONFIG_LAGUN_BITRATE,
    };
    ESP_ERROR_CHECK(lb_esp_bus_init(&bus, &cfg));

    lb_node_init(&node, &bus, &platform_impl, &identity_cfg, endpoints, KEY_COUNT);
    node.on_frame = on_frame_seen;
    /* Before lb_node_start: its first state report has to reach the LED. */
    ESP_ERROR_CHECK(lb_esp_status_init(&status, &node, CONFIG_LAGUN_SERVICE_GPIO,
                                       CONFIG_LAGUN_STATUS_LED_GPIO,
                                       STATUS_LED_ACTIVE_LOW));
    lb_node_start(&node);

    ESP_LOGI(TAG, "ShortId 0x%08lX, keys on GPIO%d, GPIO%d, GPIO%d",
             (unsigned long)lb_node_short_id(&node), key_gpio[0], key_gpio[1], key_gpio[2]);
    if (node.address == LB_ADDRESS_UNASSIGNED) {
        ESP_LOGI(TAG, "no address — announcing; the hub must have joining open");
    } else {
        ESP_LOGI(TAG, "address 0x%03X from the persistent store", node.address);
    }

    /* The initial state comes from the pins, so that starting with a key held down
     * does not look like a press. */
    uint32_t now = platform_impl.millis(platform_impl.ctx);
    bool prev_state[KEY_COUNT];
    for (int i = 0; i < KEY_COUNT; i++) {
        lb_debounce_init(&debounce[i], key_pressed(i), now);
        lb_key_init(&key_sm[i], &endpoints[i], &key_config, key_pressed(i), now);
        prev_state[i] = key_sm[i].pressed;
    }

    uint16_t prev_address = node.address;
    uint16_t attempts_made = 0;

    while (true) {
        lb_esp_bus_tick(&bus);
        lb_node_tick(&node);

        /* 2 ms: well inside the 15 ms bounce window, so a short press is never lost. */
        now = platform_impl.millis(platform_impl.ctx);
        for (int i = 0; i < KEY_COUNT; i++) {
            bool pressed = lb_debounce_update(&debounce[i], key_pressed(i), now, DEBOUNCE_MS);
            lb_key_update(&key_sm[i], &node, pressed, now);
        }
        lb_esp_status_tick(&status, now);

        /* Everything below is printed ONLY ON CHANGE — see `apps/key`. */
        for (int i = 0; i < KEY_COUNT; i++) {
            if (key_sm[i].pressed != prev_state[i]) {
                prev_state[i] = key_sm[i].pressed;
                ESP_LOGI(TAG, "key %d %s", i + 1, prev_state[i] ? "PRESSED" : "released");
            }
        }
        if (node.address != prev_address) {
            prev_address = node.address;
            ESP_LOGI(TAG, "ASSIGNMENT: address 0x%03X, node ready", node.address);
        }
        if (node.join_attempts != attempts_made) {
            attempts_made = node.join_attempts;
            ESP_LOGI(TAG, "announcement #%u, next in ~%u s",
                     node.join_attempts, node.join_backoff_s);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
