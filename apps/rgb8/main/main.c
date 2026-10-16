/* rgb8 — ESP32-C3 Super Mini + CAN transceiver + eight WS2812 in a line, ONE light.
 *
 * `Output` with `LEVEL | COLOR_RGB` (zadania/devices/DEVICE-03.md): levels, colour,
 * transitions, `LastLevel`, `PowerOnBehaviour` and storing all of it live in the library
 * (lib/lagunbus-node, `lagunbus/types/output.h`) and are tested on a computer against
 * `vectors/scenarios/output_level.json`, `output_color.json` and
 * `output_level_restore.json`. The library hands this file INTENT — the level in per mille,
 * the colour at full brightness — and what is left here is turning that into bytes on the
 * wire: White added to R, G and B (the LEDs have no white), the brightness curve, the
 * current cap, the GRB order. All of it the board's, none of it the bus's
 * (types/0x0020-output.md §5.8).
 */
#include "lagunbus/types/output.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"
#include "ws2812.h"

#include "esp_log.h"

#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "rgb8";

#define EP_LIGHT 4
#define LEDS     CONFIG_LAGUN_RGB8_LED_COUNT
/** The eye's response is roughly a power of the light, so a linear level would make half
 * the knob nearly full light (zadania/devices/DEVICE-03.md, N3). */
#define GAMMA    2.2f

static lb_attr_t attributes[] = {
    {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},
    {LB_OUT_LEVEL,              LB_T_U16,   LB_RO,              LB_NUM(0)},
    /* The colour at full brightness, persistent, white after a factory reset — 254, as
     * 0xFF is u8's "absent" (types/0x0020-output.md §5.8). */
    {LB_OUT_RED,                LB_T_U8,    LB_RO | LB_PERSIST, LB_NUM(254)},
    {LB_OUT_GREEN,              LB_T_U8,    LB_RO | LB_PERSIST, LB_NUM(254)},
    {LB_OUT_BLUE,               LB_T_U8,    LB_RO | LB_PERSIST, LB_NUM(254)},
    {LB_OUT_WHITE,              LB_T_U8,    LB_RO | LB_PERSIST, LB_NUM(0)},
    {LB_OUT_TRANSITIONING,      LB_T_BOOL,  LB_RO,              LB_NUM(0)},
    {LB_OUT_LAST_LEVEL,         LB_T_U16,   LB_RO | LB_PERSIST, LB_NUM(1000)},
    {LB_OUT_MIN_LEVEL,          LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(10)},
    {LB_OUT_MAX_LEVEL,          LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(1000)},
    {LB_OUT_DEFAULT_FADE_TIME,  LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(400)},
    {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_OFF)},
    /* No `Invert`: a LED chain has no contact and no polarity (DEVICE-03 N2). */
    {LB_OUT_AUTO_OFF_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(0)},
    {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)}, /* Light */
};

static lb_endpoint_t endpoints[] = {{
    .number = EP_LIGHT,
    .type_id = LB_TYPE_OUTPUT,
    .revision = 1,
    .features = LB_OUT_F_LEVEL | LB_OUT_F_COLOR_RGB,
    .channel = 1,
    .attrs = attributes,
    .attr_count = sizeof attributes / sizeof attributes[0],
    /* A knob bound to `SetLevel` without a transition changes the level at every detent;
     * 200 ms between rounds keeps the bus calm and loses only the values on the way. */
    .common = {.report_min_gap = 200},
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
static lb_esp_status_t status;
static lb_output_t    light_ep;
static ws2812_t       strip;

/* A Kconfig `bool` set to "no" leaves its macro undefined, so it cannot be passed on as
 * it is. */
#ifdef CONFIG_LAGUN_STATUS_LED_ACTIVE_LOW
#define STATUS_LED_ACTIVE_LOW true
#else
#define STATUS_LED_ACTIVE_LOW false
#endif

/** Intensity 0–254 → the byte for the LED: the brightness curve and the cap in one
 * table, worked out once — the C3 has no FPU, and `powf` at every step of a fade would
 * cost for nothing. */
static uint8_t curve[255];

static void build_curve(void)
{
    for (int i = 0; i < 255; i++) {
        float lit = powf((float)i / 254.0f, GAMMA) * (float)CONFIG_LAGUN_RGB8_MAX_COMPONENT;
        curve[i] = (uint8_t)lroundf(lit);
        /* A channel that is meant to shine shines: below about 30 % the curve rounds the
         * lowest levels to 0, and `MinLevel` 10 ‰ would be a dark lamp that says it is
         * on (DEVICE-03 N3). */
        if (i > 0 && curve[i] == 0) {
            curve[i] = 1;
        }
    }
}

/** The only place that touches the LEDs. `rgbw` is the colour at full brightness,
 * `level` the brightness in per mille — the library's intent. */
static void show_light(void *ctx, uint16_t level, const uint8_t *rgbw)
{
    (void)ctx;
    uint8_t rgb[3];
    for (int i = 0; i < 3; i++) {
        /* No white LED: White is added to each channel, saturating (§5.8). */
        uint32_t c = (uint32_t)rgbw[i] + rgbw[3];
        if (c > 254u) {
            c = 254u;
        }
        rgb[i] = curve[c * level / 1000u];
    }
    static uint8_t bytes[LEDS * 3];
    for (int led = 0; led < LEDS; led++) {
        bytes[led * 3 + 0] = rgb[1]; /* a WS2812B wants G, R, B */
        bytes[led * 3 + 1] = rgb[0];
        bytes[led * 3 + 2] = rgb[2];
    }
    esp_err_t err = ws2812_show(&strip, bytes, sizeof bytes);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS2812: %s", esp_err_to_name(err));
    }
}

static uint8_t handle_command(lb_endpoint_t *self, uint8_t element, const uint8_t *args,
                              uint8_t len)
{
    (void)self;
    return lb_output_handle(&light_ep, element, args, len);
}

static uint8_t handle_write(lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    (void)self;
    return lb_output_write(&light_ep, element, value);
}

void app_main(void)
{
    ESP_ERROR_CHECK(lb_esp_log_init());
    ESP_ERROR_CHECK(lb_esp_platform_init(&platform_impl));

    lb_identity_t identity_cfg = {
        .node_class = LB_CLASS_CONTROL,
        .device_type = 0x0022,
        .vendor_id = 0x0000,
        .firmware = 0x00000100,
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

    /* The LEDs first, and dark: until `PowerOnBehaviour` has decided, the light is off
     * (types/0x0020-output.md §5.7) — and a WS2812 can come up showing anything. */
    build_curve();
    ESP_ERROR_CHECK(ws2812_init(&strip, CONFIG_LAGUN_RGB8_DATA_GPIO));
    static const uint8_t dark[4] = {0, 0, 0, 0};
    show_light(NULL, 0, dark);

    endpoints[0].on_command = handle_command;
    endpoints[0].on_write = handle_write;
    endpoints[0].on_check = lb_output_check;
    endpoints[0].feature_of = lb_output_feature_of;

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

    /* AFTER lb_node_start: `LastLevel`, the colour and `PowerOnBehaviour` are restored
     * from the persistent store by then, and RESTORE reads its level there too. */
    lb_output_init_light(&light_ep, &node, &endpoints[0], show_light, NULL);

    ESP_LOGI(TAG, "ShortId 0x%08lX, %d WS2812 on GPIO%d, a channel at most %d/255",
             (unsigned long)lb_node_short_id(&node), LEDS, CONFIG_LAGUN_RGB8_DATA_GPIO,
             CONFIG_LAGUN_RGB8_MAX_COMPONENT);
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
        uint32_t now = platform_impl.millis(platform_impl.ctx);
        lb_output_tick(&light_ep, now);
        lb_esp_status_tick(&status, now);

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
