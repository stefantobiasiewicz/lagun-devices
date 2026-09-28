/* Key — ESP32-C3 Super Mini + CAN transceiver.
 *
 * This file is THIN, and that is deliberate. Everything that can be got wrong —
 * contact bounce, the multi-click window, the hold threshold — lives in
 * `components/lagunbus/key.c` and is tested on a computer (`host/test_key.c`). What is
 * left here is reading the pin and passing it on.
 */
#include "lagunbus/key.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "key";

#define EP_KEY 4

/* Endpoint attributes. The initial values are the ones from the type definition —
 * the hub will write its own while reconciling anyway, but the node has to work on
 * its own too. */
static lb_attr_t attributes[] = {
    {LB_KEY_PRESS_COUNT,        LB_T_U32,   LB_RO,              LB_NUM(0)},
    {LB_KEY_DEBOUNCE_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(15)},
    {LB_KEY_EVENT_MODE,         LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_KEY_MODE_CLICK)},
    {LB_KEY_MULTI_CLICK_WINDOW, LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(350)},
    {LB_KEY_MAX_CLICKS,         LB_T_U8,    LB_RW | LB_PERSIST, LB_NUM(3)},
    {LB_KEY_HOLD_ENABLED,       LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(1)},
    {LB_KEY_HOLD_THRESHOLD,     LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(500)},
    {LB_KEY_HOLD_TICK_INTERVAL, LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(200)},
    /* The key shorts to ground, so idle is a high level — hence Invert. */
    {LB_KEY_INVERT,             LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(1)},
};

static lb_endpoint_t endpoints[] = {{
    .number = EP_KEY,
    .type_id = LB_TYPE_KEYINPUT,
    .revision = 1,
    .features = 0,
    .channel = 1,
    .attrs = attributes,
    .attr_count = sizeof attributes / sizeof attributes[0],
}};


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
static lb_key_t       key_sm;

void app_main(void)
{
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
    lb_key_init(&key_sm, &endpoints[0], gpio_get_level(CONFIG_LAGUN_KEY_GPIO) != 0,
                now);

    uint16_t prev_address = node.address;
    bool     prev_state = key_sm.stable;
    bool     prev_hold = false;
    uint16_t attempts_made = 0;

    while (true) {
        lb_esp_bus_tick(&bus);
        lb_node_tick(&node);

        /* 2 ms. Contact bounce lasts single milliseconds, and DebounceTime has
         * the lower bound of 5 ms — polling the pin any less often, and we would start
         * losing short presses. */
        lb_key_update(&key_sm, &node,
                      gpio_get_level(CONFIG_LAGUN_KEY_GPIO) != 0,
                      platform_impl.millis(platform_impl.ctx));

        /* Everything below is printed ONLY ON CHANGE. A log that says the same thing
         * every second stops being read after five minutes — and then it also stops
         * showing what matters. */
        if (key_sm.stable != prev_state) {
            prev_state = key_sm.stable;
            ESP_LOGI(TAG, "key %s", prev_state ? "PRESSED" : "released");
        }
        if (key_sm.hold_active != prev_hold) {
            prev_hold = key_sm.hold_active;
            ESP_LOGI(TAG, "hold %s", prev_hold ? "started" : "ended");
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
