/* Dial — ESP32-C3 Super Mini + CAN transceiver + an EC11 encoder with a key in its shaft.
 *
 * This file makes the DEVICE's decisions and touches the pins; the library gives the
 * tools (lib/lagunbus-node, `value_input.h`, `key.h`, `debounce.h`, tested on a computer
 * against `vectors/scenarios/value_input*.json` and `key*.json`). The library has no
 * hardware, not even a quadrature decoder (zadania/devices/DEVICE-02.md N10): the Gray
 * code is decoded here and the library gets "n steps". The decisions:
 *
 *   - ep4 `ValueInput`: `StepSize` 20 ‰ a detent (types/0x0011-value-input.md §6), no
 *     wrap, `MinValue`/`MaxValue`/`StepSize`/`Wrap` and `ChangeCount` exposed;
 *   - ep5 `KeyInput` on the same channel — one thing a person touches: MULTICLICK and
 *     HOLD, every timing exposed, as `lagun-sim dial` (N2);
 *   - the encoder: an interrupt on both edges of A and B (N6), a table of the sixteen
 *     transitions of the Gray code — an illegal one (both changed, a bounce) counts
 *     nothing, so no time filter is needed and a fast turn is not eaten (N4);
 *     `LAGUN_ENCODER_STEPS_PER_DETENT` transitions are one step (N3);
 *   - detents counted between two loops go to the library at once, as one step of several
 *     — one `Changed` (N11); no acceleration (N5);
 *   - the shaft key shorts to ground, filtered for 15 ms — not an attribute
 *     (types/0x0010-key-input.md §5.7).
 *
 * The ESP32-C3 has no pulse counter (PCNT), which the plain ESP32 has — hence the
 * interrupt.
 */
#include "lagunbus/tools/debounce.h"
#include "lagunbus/types/key.h"
#include "lagunbus/types/value_input.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "hal/gpio_ll.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dial";

#define EP_KNOB  4
#define EP_SHAFT 5

/** 15 ms is past a mechanical key's bounce and well under the ~80 ms a finger stays
 * on it. The pin is polled every 2 ms below, so a press is never missed. */
#define DEBOUNCE_MS 15u

/** The encoder's step when nothing else is said — `StepSize` is exposed, so this is only
 * what `lb_value_input_init` falls back on if the row is ever removed. */
#define OWN_STEP 20u

static lb_attr_t knob_attributes[] = {
    LB_VI_ATTR_VALUE(),
    LB_VI_ATTR_CHANGE_COUNT(),
    LB_VI_ATTR_MIN_VALUE(0),
    LB_VI_ATTR_MAX_VALUE(1000),
    LB_VI_ATTR_STEP_SIZE(20),
    LB_VI_ATTR_WRAP(0),
};

static lb_attr_t shaft_attributes[] = {
    LB_KEY_ATTR_PRESS_COUNT(),
    LB_KEY_ATTR_EVENT_MODE(),
    LB_KEY_ATTR_MULTI_CLICK_WINDOW(350),
    LB_KEY_ATTR_MAX_CLICKS(3),
    LB_KEY_ATTR_HOLD_ENABLED(),
    LB_KEY_ATTR_HOLD_THRESHOLD(500),
    LB_KEY_ATTR_HOLD_TICK_INTERVAL(200),
};

/* The knob's hooks are set by `lb_value_input_init`, after the node restored what it
 * stored (value_input.h says why). */
static lb_endpoint_t endpoints[] = {
    {
        .number = EP_KNOB,
        .type_id = LB_TYPE_VALUEINPUT,
        .revision = 1,
        .channel = 1,
        .attrs = knob_attributes,
        .attr_count = sizeof knob_attributes / sizeof knob_attributes[0],
    },
    {
        .number = EP_SHAFT,
        .type_id = LB_TYPE_KEYINPUT,
        .revision = 1,
        .features = LB_KEY_F_MULTICLICK | LB_KEY_F_HOLD,
        .channel = 1,
        .attrs = shaft_attributes,
        .attr_count = sizeof shaft_attributes / sizeof shaft_attributes[0],
        .on_check = lb_key_check,
        .feature_of = lb_key_feature_of,
    },
};

static const lb_key_config_t key_config = LB_KEY_CONFIG_DEFAULT;

/* --- the encoder --------------------------------------------------------------------- */

/** The move from the previous A,B to the current one, indexed `previous << 2 | current`:
 * +1 one way, −1 the other, 0 for no change and for the four where both pins changed —
 * a bounce or a missed edge, which cannot say a direction and so counts nothing. In
 * DRAM, as everything the interrupt reads: a `const` table would land in flash. */
static const DRAM_ATTR int8_t GRAY[16] = {
    0, -1, +1, 0,
    +1, 0, 0, -1,
    -1, 0, 0, +1,
    0, +1, -1, 0,
};

static volatile int32_t transitions;  /**< counted by the interrupt, taken by the loop */
static volatile uint8_t last_ab;
static portMUX_TYPE     encoder_lock = portMUX_INITIALIZER_UNLOCKED;

static inline uint8_t read_ab(void)
{
    /* `gpio_ll` and not `gpio_get_level`: the latter is not in IRAM by default, and an
     * interrupt running from flash crashes while the cache is off for an NVS write —
     * the very moment the knob's value is being stored. */
    uint8_t a = (uint8_t)gpio_ll_get_level(&GPIO, CONFIG_LAGUN_ENCODER_A_GPIO);
    uint8_t b = (uint8_t)gpio_ll_get_level(&GPIO, CONFIG_LAGUN_ENCODER_B_GPIO);
    return (uint8_t)(a << 1 | b);
}

static void IRAM_ATTR on_edge(void *arg)
{
    (void)arg;
    uint8_t ab = read_ab();
    portENTER_CRITICAL_ISR(&encoder_lock);
    transitions += GRAY[(last_ab << 2) | ab];
    last_ab = ab;
    portEXIT_CRITICAL_ISR(&encoder_lock);
}

static int32_t take_transitions(void)
{
    portENTER_CRITICAL(&encoder_lock);
    int32_t n = transitions;
    transitions = 0;
    portEXIT_CRITICAL(&encoder_lock);
    return n;
}

#ifdef CONFIG_LAGUN_ENCODER_REVERSE
#define DIRECTION (-1)
#else
#define DIRECTION 1
#endif

/** Whole detents out of the transitions so far; the rest waits for the next loop — a
 * knob stopped between two detents moves nothing. */
static int32_t take_detents(void)
{
    static int32_t partial;
    partial += take_transitions();
    int32_t detents = partial / CONFIG_LAGUN_ENCODER_STEPS_PER_DETENT;
    partial -= detents * CONFIG_LAGUN_ENCODER_STEPS_PER_DETENT;
    return DIRECTION * detents;
}

static void encoder_init(void)
{
    gpio_config_t pins = {
        .pin_bit_mask = 1ULL << CONFIG_LAGUN_ENCODER_A_GPIO | 1ULL << CONFIG_LAGUN_ENCODER_B_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&pins));
    last_ab = read_ab();
    /* Installed once per chip; another part of the firmware may have done it already. */
    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_LAGUN_ENCODER_A_GPIO, on_edge, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_LAGUN_ENCODER_B_GPIO, on_edge, NULL));
}

/* --- the shaft key ------------------------------------------------------------------- */

/** The finger is on the shaft. The key shorts to ground: a LOW pin is a press. */
static bool key_pressed(void)
{
    return gpio_get_level(CONFIG_LAGUN_KEY_GPIO) == 0;
}

/** Print a frame with its meaning, not just its bytes — as `apps/key` does. */
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

static lb_transport_t   bus;
static lb_platform_t    platform_impl;
static lb_node_t        node;
static lb_esp_status_t  status;
static lb_value_input_t knob;
static lb_key_t         shaft;
static lb_debounce_t    debounce;

/* A Kconfig `bool` set to "no" leaves its macro undefined, so it cannot be passed on as
 * it is. */
#ifdef CONFIG_LAGUN_STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW true
#else
#define STATUS_LED_ACTIVE_LOW false
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(lb_esp_log_init());
    ESP_ERROR_CHECK(lb_esp_platform_init(&platform_impl));

    lb_identity_t identity_cfg = {
        .node_class = LB_CLASS_CONTROL,  /* it controls something -> block 0x001..0x0FF */
        .device_type = 0x0012,           /* devices/README.md § Models */
        .vendor_id = 0x0000,
        .firmware = 0x00000100,          /* 0.1.0 */
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

    gpio_config_t key = {
        .pin_bit_mask = 1ULL << CONFIG_LAGUN_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&key));
    encoder_init();

    lb_esp_bus_cfg_t cfg = {
        .tx = CONFIG_LAGUN_TWAI_TX,
        .rx = CONFIG_LAGUN_TWAI_RX,
        .bitrate = CONFIG_LAGUN_BITRATE,
    };
    ESP_ERROR_CHECK(lb_esp_bus_init(&bus, &cfg));

    /* The type's reporting defaults, before the node restores stored ones over them. */
    lb_value_input_defaults(&endpoints[0]);
    lb_node_init(&node, &bus, &platform_impl, &identity_cfg, endpoints,
                 (uint8_t)(sizeof endpoints / sizeof endpoints[0]));
    node.on_frame = on_frame_seen;
    /* Before lb_node_start: its first state report has to reach the LED. */
    ESP_ERROR_CHECK(lb_esp_status_init(&status, &node, CONFIG_LAGUN_SERVICE_GPIO,
                                       CONFIG_LAGUN_STATUS_LED_GPIO,
                                       STATUS_LED_ACTIVE_LOW));
    lb_node_start(&node);

    uint32_t now = platform_impl.millis(platform_impl.ctx);
    lb_value_input_init(&knob, &node, &endpoints[0], OWN_STEP);
    lb_debounce_init(&debounce, key_pressed(), now);
    lb_key_init(&shaft, &endpoints[1], &key_config, key_pressed(), now);
    take_transitions();  /* whatever the pins did while starting is not a turn */

    ESP_LOGI(TAG, "ShortId 0x%08lX, encoder on GPIO%d/%d, key on GPIO%d, value %u ‰",
             (unsigned long)lb_node_short_id(&node), CONFIG_LAGUN_ENCODER_A_GPIO,
             CONFIG_LAGUN_ENCODER_B_GPIO, CONFIG_LAGUN_KEY_GPIO,
             lb_value_input_value(&knob));
    if (node.address == LB_ADDRESS_UNASSIGNED) {
        ESP_LOGI(TAG, "no address — announcing; the hub must have joining open");
    } else {
        ESP_LOGI(TAG, "address 0x%03X from the persistent store", node.address);
    }

    uint16_t prev_address = node.address;
    uint16_t prev_value = lb_value_input_value(&knob);
    bool     prev_pressed = shaft.pressed;
    uint16_t attempts_made = 0;

    while (true) {
        lb_esp_bus_tick(&bus);
        lb_node_tick(&node);

        now = platform_impl.millis(platform_impl.ctx);
        int32_t detents = take_detents();
        if (detents != 0) {
            lb_value_input_step(&knob, detents);
        }
        lb_value_input_tick(&knob, now);
        bool pressed = lb_debounce_update(&debounce, key_pressed(), now, DEBOUNCE_MS);
        lb_key_update(&shaft, &node, pressed, now);
        lb_esp_status_tick(&status, now);

        /* Printed ONLY ON CHANGE, as in `apps/key`. */
        uint16_t value = lb_value_input_value(&knob);
        if (value != prev_value) {
            prev_value = value;
            ESP_LOGI(TAG, "value %u ‰", value);
        }
        if (shaft.pressed != prev_pressed) {
            prev_pressed = shaft.pressed;
            ESP_LOGI(TAG, "shaft %s", prev_pressed ? "PRESSED" : "released");
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
