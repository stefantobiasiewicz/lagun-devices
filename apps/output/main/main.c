/* Output — ESP32-C3 Super Mini + CAN transceiver + relay module.
 *
 * `PowerOnBehaviour`, `AutoOffTime` and `MinGap`-limited reports live in the library
 * (lib/lagunbus-node, `lagunbus/types/output.h`) and are tested on a computer against
 * `vectors/scenarios/output.json`. What is left here is the relay pin and the LED.
 */
#include "lagunbus/types/output.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "output";

#define EP_OUTPUT 4

static lb_attr_t attributes[] = {
    {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},
    {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_OFF)},
    /* A typical relay module switches on with a LOW level. Check yours. */
    {LB_OUT_INVERT,             LB_T_BOOL,  LB_RW | LB_PERSIST, LB_NUM(1)},
    {LB_OUT_AUTO_OFF_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(0)},
    {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)}, /* Light */
    /* Without this, a held-down key toggling the output would flood the bus
     * with reports. 200 ms does not lose the last state, only the ones on the way. */
    {LB_COMMON_REPORTMINGAP,    LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(200)},
};

static lb_endpoint_t endpoints[] = {{
    .number = EP_OUTPUT,
    .type_id = LB_TYPE_OUTPUT,
    .revision = 1,
    .features = 0,  /* no dimming and no colour — a plain relay */
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
static lb_output_t    output_ep;

/** The only place that touches hardware. `level` already has `Invert` applied. */
static void drive_output(void *ctx, bool level)
{
    (void)ctx;
    gpio_set_level(CONFIG_LAGUN_RELAY_GPIO, level ? 1 : 0);
    bool on = lb_endpoint_attr(&endpoints[0], LB_OUT_ON_OFF)->value.value != 0;
    /* We print both the logical state and the pin level. A mismatch between them means
     * a badly set `Invert` — a fault that everywhere else looks like a perfectly
     * working relay, only inverted. */
    ESP_LOGI(TAG, "output %s (GPIO%d = %d)", on ? "ON" : "off",
             CONFIG_LAGUN_RELAY_GPIO, level ? 1 : 0);
#if CONFIG_LAGUN_LED_GPIO >= 0
    /* The LED on the Super Mini lights on a low level, so the other way round from the
     * relay once its own Invert is taken into account. It shows the output's LOGICAL
     * state. */
    bool on_state = lb_endpoint_attr(&endpoints[0], LB_OUT_ON_OFF)->value.value != 0;
    gpio_set_level(CONFIG_LAGUN_LED_GPIO, on_state ? 0 : 1);
#endif
}

static uint8_t handle_command(lb_endpoint_t *self, uint8_t element, const uint8_t *args,
                       uint8_t len)
{
    (void)self;
    (void)args;
    (void)len;
    return lb_output_command(&output_ep, element);
}

void app_main(void)
{
    ESP_ERROR_CHECK(lb_esp_platform_init(&platform_impl));

    lb_identity_t identity_cfg = {
        .node_class = LB_CLASS_CONTROL,
        .device_type = 0x0020,
        .vendor_id = 0x0000,
        .firmware = 0x00000100,
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

    uint64_t pins = 1ULL << CONFIG_LAGUN_RELAY_GPIO;
#if CONFIG_LAGUN_LED_GPIO >= 0
    pins |= 1ULL << CONFIG_LAGUN_LED_GPIO;
#endif
    gpio_config_t out_ep = {
        .pin_bit_mask = pins,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_ep));

    endpoints[0].on_command = handle_command;

    lb_esp_bus_cfg_t cfg = {
        .tx = CONFIG_LAGUN_TWAI_TX,
        .rx = CONFIG_LAGUN_TWAI_RX,
        .bitrate = CONFIG_LAGUN_BITRATE,
    };
    ESP_ERROR_CHECK(lb_esp_bus_init(&bus, &cfg));

    lb_node_init(&node, &bus, &platform_impl, &identity_cfg, endpoints, 1);
    node.on_frame = on_frame_seen;
    lb_node_start(&node);

    /* AFTER lb_node_start, because RESTORE reads the state from the persistent store,
     * and that is only available by then. */
    lb_output_init(&output_ep, &node, &endpoints[0], drive_output, NULL);

    ESP_LOGI(TAG, "ShortId 0x%08lX, relay on GPIO%d",
             (unsigned long)lb_node_short_id(&node), CONFIG_LAGUN_RELAY_GPIO);
    if (node.address == LB_ADDRESS_UNASSIGNED) {
        ESP_LOGI(TAG, "no address — announcing; the hub must have joining open");
    } else {
        ESP_LOGI(TAG, "address 0x%03X from the persistent store", node.address);
    }

    uint16_t prev_address = node.address;
    uint16_t attempts_made = 0;
    while (true) {
        lb_esp_bus_tick(&bus);
        lb_node_tick(&node);
        lb_output_tick(&output_ep, platform_impl.millis(platform_impl.ctx));

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
