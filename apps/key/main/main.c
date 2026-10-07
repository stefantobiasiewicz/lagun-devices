/* Key — ESP32-C3 Super Mini + CAN transceiver. One key, with multi-click.
 *
 * This file makes the DEVICE's decisions and nothing else; the library gives the tools
 * (lib/lagunbus-node, `key.h` and `debounce.h`, tested on a computer against
 * `vectors/scenarios/key*.json`). The decisions, all in one place below:
 *
 *   - features: MULTICLICK only — `Click(1…MaxClicks)`, no hold, no RAW;
 *   - exposed to the hub: `EventMode` (CLICK or MULTICLICK), the multi-click window and
 *     `MaxClicks`, and `PressCount`;
 *   - polarity: the key shorts to ground, so a LOW pin is a press;
 *   - bounce: 15 ms, filtered here — not an attribute (types/0x0010-key-input.md §5.7).
 *
 * The type's default mode is CLICK; the hub switches this key to MULTICLICK through
 * `EventMode` in its configuration.
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

static const char *TAG = "key";

#define EP_KEY 4

/** 15 ms is past a mechanical key's bounce and well under the ~80 ms a finger stays
 * on it. The pin is polled every 2 ms below, so a press is never missed. */
#define DEBOUNCE_MS 15u

/* What the hub may see and tune. The initial values are the type's defaults — the hub
 * writes its own while reconciling, but the node has to work on its own too. */
static lb_attr_t attributes[] = {
    LB_KEY_ATTR_PRESS_COUNT(),
    LB_KEY_ATTR_EVENT_MODE(),
    LB_KEY_ATTR_MULTI_CLICK_WINDOW(350),
    LB_KEY_ATTR_MAX_CLICKS(3),
};

static lb_endpoint_t endpoints[] = {{
    .number = EP_KEY,
    .type_id = LB_TYPE_KEYINPUT,
    .revision = 1,
    .features = LB_KEY_F_MULTICLICK,
    .channel = 1,
    .attrs = attributes,
    .attr_count = sizeof attributes / sizeof attributes[0],
    .on_check = lb_key_check,
    .feature_of = lb_key_feature_of,
}};

/* The rest of the key's timings. With no HOLD feature the hold values are never used;
 * the multi-click ones apply only if the attributes above were removed. */
static const lb_key_config_t key_config = LB_KEY_CONFIG_DEFAULT;

/** The finger is on the key. The key shorts to ground: a LOW pin is a press. */
static bool key_pressed(void)
{
    return gpio_get_level(CONFIG_LAGUN_KEY_GPIO) == 0;
}


/** Print a frame with its meaning, not just its bytes.
 *
 * During a first bring-up this is the only window onto what is happening: whether the
 * announcement went out, whether the hub answered, whether anything travels the
 * bus. `candump` on the RPi shows the same thing, but from the other end of the cable —
 * and the difference between those two views points at where the fault is.
 */
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
static lb_key_t       key_sm;
static lb_debounce_t  debounce;

void app_main(void)
{
    ESP_ERROR_CHECK(lb_esp_log_init());
    ESP_ERROR_CHECK(lb_esp_platform_init(&platform_impl));

    lb_identity_t identity_cfg = {
        .node_class = LB_CLASS_CONTROL,  /* it controls something -> block 0x001..0x0FF */
        .device_type = 0x0010,
        .vendor_id = 0x0000,
        .firmware = 0x00000100,          /* 0.1.0 */
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << CONFIG_LAGUN_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    lb_esp_bus_cfg_t cfg = {
        .tx = CONFIG_LAGUN_TWAI_TX,
        .rx = CONFIG_LAGUN_TWAI_RX,
        .bitrate = CONFIG_LAGUN_BITRATE,
    };
    ESP_ERROR_CHECK(lb_esp_bus_init(&bus, &cfg));

    lb_node_init(&node, &bus, &platform_impl, &identity_cfg, endpoints, 1);
    node.on_frame = on_frame_seen;
    /* Before lb_node_start: its first state report has to reach the LED. */
    ESP_ERROR_CHECK(lb_esp_status_init(&status, &node, CONFIG_LAGUN_SERVICE_GPIO,
                                       CONFIG_LAGUN_STATUS_LED_GPIO,
                                       STATUS_LED_ACTIVE_LOW));
    lb_node_start(&node);

    ESP_LOGI(TAG, "ShortId 0x%08lX, key on GPIO%d",
             (unsigned long)lb_node_short_id(&node), CONFIG_LAGUN_KEY_GPIO);
    if (node.address == LB_ADDRESS_UNASSIGNED) {
        ESP_LOGI(TAG, "no address — announcing; the hub must have joining open");
    } else {
        ESP_LOGI(TAG, "address 0x%03X from the persistent store", node.address);
    }

    /* The initial state comes from the pin, so that starting with the key held down
     * does not look like a press. */
    uint32_t now = platform_impl.millis(platform_impl.ctx);
    lb_debounce_init(&debounce, key_pressed(), now);
    lb_key_init(&key_sm, &endpoints[0], &key_config, key_pressed(), now);

    uint16_t prev_address = node.address;
    bool     prev_state = key_sm.pressed;
    uint16_t attempts_made = 0;

    while (true) {
        lb_esp_bus_tick(&bus);
        lb_node_tick(&node);

        /* 2 ms: well inside the 15 ms bounce window, so a short press is never lost. */
        now = platform_impl.millis(platform_impl.ctx);
        bool pressed = lb_debounce_update(&debounce, key_pressed(), now, DEBOUNCE_MS);
        lb_key_update(&key_sm, &node, pressed, now);
        lb_esp_status_tick(&status, now);

        /* Everything below is printed ONLY ON CHANGE. A log that says the same thing
         * every second stops being read after five minutes — and then it also stops
         * showing what matters. */
        if (key_sm.pressed != prev_state) {
            prev_state = key_sm.pressed;
            ESP_LOGI(TAG, "key %s", prev_state ? "PRESSED" : "released");
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
