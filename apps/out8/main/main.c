/* Out8 — ESP32-C3 Super Mini + CAN transceiver + MCP23017. Eight outputs.
 *
 * `apps/output` eight times over, on an I²C port expander instead of a GPIO. Everything
 * an output does — `PowerOnBehaviour`, `AutoOffTime`, `MinGap`-limited reports — lives in
 * the library (lib/lagunbus-node, `lagunbus/types/output.h`), tested on a computer; the
 * library has eight outputs in one node already (`output8.json`). What is new here is
 * only where `drive` puts the level: a bit of the MCP23017's port A.
 *
 * The decisions (zadania/zrobione/FEAT-09.md):
 *
 *   - on a breadboard for now: LEDs straight from GPA0…GPA7, lit by a HIGH pin
 *     (N4); no `Invert` since zadania/zrobione/CR-04.md (N7). Relays later;
 *   - I²C is assumed to work — no retries, no fault handling (N5);
 *   - the MCP23017's RESET is tied to 3V3, so a restart of the ESP32 alone does not reset
 *     it; the registers are written in full before every start instead (N6);
 *   - `DeviceType` 0x0021 — a model of its own (devices/README.md § Models).
 */
#include "lagunbus/types/output.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"
#include "mcp23017.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "out8";

#define OUTPUT_COUNT 8
#define EP_FIRST     4

/* --- I²C and the MCP23017 ------------------------------------------------------------ */

static mcp23017_t mcp;

static void mcp_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,  /* any free controller — the C3 has one */
        .sda_io_num = CONFIG_LAGUN_I2C_SDA,
        .scl_io_num = CONFIG_LAGUN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* The 4.7 kΩ resistors on the board do the real work; the internal pull-ups
         * (~45 kΩ) only keep the lines high if they are missing. */
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    ESP_ERROR_CHECK(mcp23017_init(&mcp, i2c_bus, CONFIG_LAGUN_MCP_ADDRESS, 400000));
}

/** N5: I²C is assumed to work. A failed write is logged and nothing more. */
static void mcp_check(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MCP23017 %s: %s", what, esp_err_to_name(err));
    }
}

/* --- the outputs --------------------------------------------------------------------- */

#define OUTPUT_ATTRIBUTES                                                                  \
    {                                                                                      \
        {LB_OUT_ON_OFF,             LB_T_BOOL,  LB_RO,              LB_NUM(0)},            \
        {LB_OUT_POWER_ON_BEHAVIOUR, LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(LB_POWER_ON_OFF)}, \
        /* No `Invert`: an LED from the pin to ground lights on HIGH, and that is build, \
         * not something for the hub to write (zadania/zrobione/CR-04.md, N7). */       \
        {LB_OUT_AUTO_OFF_TIME,      LB_T_U16,   LB_RW | LB_PERSIST, LB_NUM(0)},            \
        {LB_OUT_PURPOSE,            LB_T_ENUM8, LB_RW | LB_PERSIST, LB_NUM(1)}, /* Light */ \
    }
static lb_attr_t attributes[OUTPUT_COUNT][4] = {
    OUTPUT_ATTRIBUTES, OUTPUT_ATTRIBUTES, OUTPUT_ATTRIBUTES, OUTPUT_ATTRIBUTES,
    OUTPUT_ATTRIBUTES, OUTPUT_ATTRIBUTES, OUTPUT_ATTRIBUTES, OUTPUT_ATTRIBUTES,
};

#define OUTPUT_ENDPOINT(i)                                            \
    {                                                                 \
        .number = (uint8_t)(EP_FIRST + (i)),                          \
        .type_id = LB_TYPE_OUTPUT,                                    \
        .revision = 1,                                                \
        .features = 0, /* no dimming and no colour — on and off */    \
        .channel = (uint8_t)(1 + (i)),                                \
        .attrs = attributes[i],                                       \
        .attr_count = sizeof attributes[i] / sizeof attributes[i][0], \
        /* As apps/output: a held-down key must not flood the bus. */  \
        .common = {.report_min_gap = 200},                            \
    }
static lb_endpoint_t endpoints[OUTPUT_COUNT] = {
    OUTPUT_ENDPOINT(0), OUTPUT_ENDPOINT(1), OUTPUT_ENDPOINT(2), OUTPUT_ENDPOINT(3),
    OUTPUT_ENDPOINT(4), OUTPUT_ENDPOINT(5), OUTPUT_ENDPOINT(6), OUTPUT_ENDPOINT(7),
};

static lb_output_t outputs[OUTPUT_COUNT];

/** The only place that touches the outputs. `level` is the output's state — the LEDs
 * light on HIGH, so it goes to the pin as it is; `ctx` is the output's index,
 * 0 … 7 = GPA0 … GPA7. Relays driven LOW would invert it here. */
static void drive_output(void *ctx, bool level)
{
    int i = (int)(intptr_t)ctx;
    mcp_check(mcp23017_write_pin(&mcp, MCP23017_PORT_A, (uint8_t)i, level), "write");
    bool on = lb_endpoint_attr(&endpoints[i], LB_OUT_ON_OFF)->value.value != 0;
    /* Both the logical state and the pin level: a mismatch means the board's polarity is
     * wrong here (see apps/output). */
    ESP_LOGI(TAG, "output %d %s (GPA%d = %d)", i + 1, on ? "ON" : "off", i, level ? 1 : 0);
}

static uint8_t handle_command(lb_endpoint_t *self, uint8_t element, const uint8_t *args,
                              uint8_t len)
{
    (void)args;
    (void)len;
    return lb_output_command(&outputs[self->number - EP_FIRST], element);
}

/** Every output OFF in the latch, then port A to outputs — in this order, so no output
 * shows anything on its way to `PowerOnBehaviour` (N6).
 *
 * After power-on the MCP23017's pins are inputs, and an LED (or a ULN2803A later) sees
 * nothing on them: dark. After a restart of the ESP32 alone the MCP23017 keeps its
 * registers — RESET is tied to 3V3 — so the outputs hold their last state until here;
 * this then switches them off for the moment before `lb_output_init` applies
 * `PowerOnBehaviour`. Written in full every time, never trusted to be as we left it. */
static void outputs_off_then_enable(void)
{
    /* Every LED lights on HIGH, so off is LOW on all eight. The polarity is the board's,
     * fixed here with `drive_output` — there is no `Invert` to read
     * (zadania/zrobione/CR-04.md, N7). Relays driven LOW would make this 0xFF. */
    const uint8_t off = 0x00;
    mcp_check(mcp23017_write_port(&mcp, MCP23017_PORT_A, off), "latch");
    mcp_check(mcp23017_set_direction(&mcp, MCP23017_PORT_A, 0x00), "direction");
}

/* --- the node ------------------------------------------------------------------------ */

/** Print a frame with its meaning, not just its bytes — as in `apps/output`, where the
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

void app_main(void)
{
    ESP_ERROR_CHECK(lb_esp_log_init());
    ESP_ERROR_CHECK(lb_esp_platform_init(&platform_impl));

    lb_identity_t identity_cfg = {
        .node_class = LB_CLASS_CONTROL,
        .device_type = 0x0021,  /* out8 — devices/README.md § Models */
        .vendor_id = 0x0000,
        .firmware = 0x00000100,
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

    mcp_init();

    for (int i = 0; i < OUTPUT_COUNT; i++) {
        endpoints[i].on_command = handle_command;
    }

    lb_esp_bus_cfg_t cfg = {
        .tx = CONFIG_LAGUN_TWAI_TX,
        .rx = CONFIG_LAGUN_TWAI_RX,
        .bitrate = CONFIG_LAGUN_BITRATE,
    };
    ESP_ERROR_CHECK(lb_esp_bus_init(&bus, &cfg));

    lb_node_init(&node, &bus, &platform_impl, &identity_cfg, endpoints, OUTPUT_COUNT);
    node.on_frame = on_frame_seen;
    /* Before lb_node_start: its first state report has to reach the LED. */
    ESP_ERROR_CHECK(lb_esp_status_init(&status, &node, CONFIG_LAGUN_SERVICE_GPIO,
                                       CONFIG_LAGUN_STATUS_LED_GPIO,
                                       STATUS_LED_ACTIVE_LOW));
    lb_node_start(&node);

    /* AFTER lb_node_start, as in apps/output: RESTORE reads the state from the
     * persistent store, available only by then. The latch goes first (N6). */
    outputs_off_then_enable();
    for (int i = 0; i < OUTPUT_COUNT; i++) {
        lb_output_init(&outputs[i], &node, &endpoints[i], drive_output, (void *)(intptr_t)i);
    }

    ESP_LOGI(TAG, "ShortId 0x%08lX, MCP23017 0x%02X on SDA GPIO%d, SCL GPIO%d",
             (unsigned long)lb_node_short_id(&node), CONFIG_LAGUN_MCP_ADDRESS,
             CONFIG_LAGUN_I2C_SDA, CONFIG_LAGUN_I2C_SCL);
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
        for (int i = 0; i < OUTPUT_COUNT; i++) {
            lb_output_tick(&outputs[i], now);
        }
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
