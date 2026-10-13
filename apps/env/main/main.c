/* Env — ESP32-C3 Super Mini + CAN transceiver + SHT40 + VEML7700. A room sensor.
 *
 * Three `Sense` endpoints: temperature on ep4, humidity on ep5, light on ep6, each with
 * `MIN_MAX`, `THRESHOLD` and `Offset`. Everything a sensor does on the bus — "absent"
 * until the first reading, `Offset`, extremes and their storing, zones and `Threshold`,
 * reports with hysteresis (relative for light) — lives in the library
 * (lib/lagunbus-node, `lagunbus/types/sense.h`), tested on a computer with the same
 * device as `env` in its catalogue. What is here is only when and how the chips are read.
 *
 * The decisions (zadania/zrobione/DEVICE-01.md):
 *
 *   - SHT40 every 2 s, VEML7700 every second (N3): reports are cut by hysteresis and
 *     `ReportMinGap` anyway, the measuring period is the device's, not an attribute;
 *   - the VEML7700 picks its own range (N4, components/veml7700);
 *   - no SHT40 heater (N6);
 *   - a failed transfer or CRC is `lb_sense_fail` — "absent" on the bus, never a zero —
 *     and three in a row reset the chip (N7);
 *   - `DeviceType` 0x0030 (N9, devices/README.md § Models), class SENSOR.
 */
#include "lagunbus/types/sense.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"
#include "sht4x.h"
#include "veml7700.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "env";

#define SHT_PERIOD_MS   2000u
#define LIGHT_PERIOD_MS 1000u
#define LOG_PERIOD_MS   10000u
/** N7: this many failures in a row and the chip is reset. */
#define FAILURES_TO_RESET 3u

/* --- the endpoints ------------------------------------------------------------------- */

#define SENSE_ROWS(quantity)                \
    {                                       \
        LB_SNS_ATTR_VALUE(),                \
        LB_SNS_ATTR_QUANTITY(quantity),     \
        LB_SNS_ATTR_MIN_MEASURED(),         \
        LB_SNS_ATTR_MAX_MEASURED(),         \
        LB_SNS_ATTR_OFFSET(),               \
        LB_SNS_ATTR_THRESHOLD_LOW(),        \
        LB_SNS_ATTR_THRESHOLD_HIGH(),       \
        LB_SNS_ATTR_THRESHOLD_HYSTERESIS(), \
    }
static lb_attr_t temperature_rows[] = SENSE_ROWS(LB_QUANTITY_TEMPERATURE);
static lb_attr_t humidity_rows[] = SENSE_ROWS(LB_QUANTITY_HUMIDITY);
static lb_attr_t light_rows[] = SENSE_ROWS(LB_QUANTITY_ILLUMINANCE);

/* Channel 0: the sensors measure the room, not the effect of a channel
 * (types/0x0030-sense.md §5.6). */
#define SENSE_ENDPOINT(n, rows)                           \
    {                                                     \
        .number = (n),                                    \
        .type_id = LB_TYPE_SENSE,                         \
        .revision = 1,                                    \
        .features = LB_SNS_F_MIN_MAX | LB_SNS_F_THRESHOLD, \
        .channel = 0,                                     \
        .attrs = (rows),                                  \
        .attr_count = sizeof(rows) / sizeof((rows)[0]),   \
        .feature_of = lb_sense_feature_of,                \
    }
static lb_endpoint_t endpoints[] = {
    SENSE_ENDPOINT(4, temperature_rows),
    SENSE_ENDPOINT(5, humidity_rows),
    SENSE_ENDPOINT(6, light_rows),
};
#define ENDPOINT_COUNT (sizeof endpoints / sizeof endpoints[0])

static lb_sense_t temperature, humidity, light;

/* --- the chips ----------------------------------------------------------------------- */

static sht4x_t    sht;
static veml7700_t veml;

static void sensors_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,  /* any free controller — the C3 has one */
        .sda_io_num = CONFIG_LAGUN_I2C_SDA,
        .scl_io_num = CONFIG_LAGUN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* The modules' own pull-ups (or 4.7 kΩ on the breadboard) do the real work; the
         * internal ones (~45 kΩ) only keep the lines high if those are missing. */
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    /* 100 kHz: both chips do 400, but two modules on jumper wires are happier slower, and
     * three readings a second need no speed. */
    ESP_ERROR_CHECK(sht4x_init(&sht, i2c_bus, SHT4X_ADDRESS, 100000));
    ESP_ERROR_CHECK(veml7700_init(&veml, i2c_bus, VEML7700_ADDRESS, 100000));
    /* Not fatal: a missing chip is "absent" on the bus, and the loop retries (N7). */
    esp_err_t err = veml7700_configure(&veml);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "VEML7700 configure: %s", esp_err_to_name(err));
    }
}

/** One chip's failures in a row (N7). */
typedef struct {
    const char *name;
    unsigned    failures;
} health_t;

static health_t sht_health = {.name = "SHT40"};
static health_t veml_health = {.name = "VEML7700"};

/** A failed reading: "absent" to the library, and after three a reset of the chip. */
static bool failed(health_t *h, esp_err_t err)
{
    h->failures++;
    ESP_LOGW(TAG, "%s: %s (%u in a row)", h->name, esp_err_to_name(err), h->failures);
    if (h->failures < FAILURES_TO_RESET) {
        return false;
    }
    h->failures = 0;
    return true;
}

/** The SHT40: started, then fetched `SHT4X_MEASURE_MS` later — the loop never sleeps
 * through a conversion. */
static void sht_tick(uint32_t now)
{
    static uint32_t started_ms;
    static bool     measuring;
    static uint32_t next_ms;

    if (measuring && now - started_ms >= SHT4X_MEASURE_MS) {
        measuring = false;
        sht4x_reading_t r;
        esp_err_t err = sht4x_fetch(&sht, &r);
        if (err == ESP_OK) {
            sht_health.failures = 0;
            lb_sense_update(&temperature, r.temperature);
            lb_sense_update(&humidity, r.humidity);
        } else {
            lb_sense_fail(&temperature);
            lb_sense_fail(&humidity);
            if (failed(&sht_health, err)) {
                sht4x_reset(&sht); /* ready 1 ms later; the next start is 2 s away */
            }
        }
    }
    if (!measuring && (int32_t)(now - next_ms) >= 0) {
        next_ms = now + SHT_PERIOD_MS;
        esp_err_t err = sht4x_start(&sht);
        if (err == ESP_OK) {
            started_ms = now;
            measuring = true;
        } else {
            lb_sense_fail(&temperature);
            lb_sense_fail(&humidity);
            if (failed(&sht_health, err)) {
                sht4x_reset(&sht);
            }
        }
    }
}

static void light_tick(uint32_t now)
{
    static uint32_t next_ms;
    if ((int32_t)(now - next_ms) < 0) {
        return;
    }
    uint32_t period = veml7700_integration_ms(&veml);
    next_ms = now + (period > LIGHT_PERIOD_MS ? period : LIGHT_PERIOD_MS);
    uint32_t centilux = 0;
    esp_err_t err = veml7700_read(&veml, &centilux);
    if (err == ESP_OK) {
        veml_health.failures = 0;
        /* `u32` on the wire is writable only to INT32_MAX (lagunbus-node README
         * § Decisions); 120 klx is 12 000 000 hundredths — far below. */
        lb_sense_update(&light, (int32_t)centilux);
    } else if (err == VEML7700_SETTLING) {
        return; /* a change of range — not a failure, nothing new to say */
    } else {
        lb_sense_fail(&light);
        if (failed(&veml_health, err)) {
            veml7700_configure(&veml); /* the chip has no reset command */
        }
    }
}

/** What the sensors read, now and then — the bus says it too, but the log is quicker. */
static void log_tick(uint32_t now)
{
    static uint32_t next_ms = LOG_PERIOD_MS;
    if ((int32_t)(now - next_ms) < 0) {
        return;
    }
    next_ms = now + LOG_PERIOD_MS;
    lb_value_t t = lb_sense_value(&temperature);
    lb_value_t h = lb_sense_value(&humidity);
    lb_value_t l = lb_sense_value(&light);
    char tt[16] = "--", hh[16] = "--", ll[16] = "--";
    if (!t.absent) {
        long v = labs((long)t.value); /* the sign apart: −0.50 °C has no whole degrees */
        snprintf(tt, sizeof tt, "%s%ld.%02ld", t.value < 0 ? "-" : "", v / 100, v % 100);
    }
    if (!h.absent) {
        snprintf(hh, sizeof hh, "%ld.%02ld", (long)(h.value / 100), (long)(h.value % 100));
    }
    if (!l.absent) {
        snprintf(ll, sizeof ll, "%lu", (unsigned long)((uint32_t)l.value / 100u));
    }
    ESP_LOGI(TAG, "%s °C  %s %%  %s lx", tt, hh, ll);
}

/* --- the node ------------------------------------------------------------------------ */

/** Print a frame with its meaning, not just its bytes — as in `apps/out8`. */
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

static lb_transport_t  bus;
static lb_platform_t   platform_impl;
static lb_node_t       node;
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
        .node_class = LB_CLASS_SENSOR,
        .device_type = 0x0030,  /* env — devices/README.md § Models */
        .vendor_id = 0x0000,
        .firmware = 0x00000100,
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

    sensors_init();

    /* BEFORE lb_node_start: the `⟨Q⟩` rows get their types here, and the node needs them
     * to restore stored thresholds and extremes (lagunbus-node README § Decisions). */
    lb_sense_defaults(&endpoints[0], LB_QUANTITY_TEMPERATURE);
    lb_sense_defaults(&endpoints[1], LB_QUANTITY_HUMIDITY);
    lb_sense_defaults(&endpoints[2], LB_QUANTITY_ILLUMINANCE);

    lb_esp_bus_cfg_t cfg = {
        .tx = CONFIG_LAGUN_TWAI_TX,
        .rx = CONFIG_LAGUN_TWAI_RX,
        .bitrate = CONFIG_LAGUN_BITRATE,
    };
    ESP_ERROR_CHECK(lb_esp_bus_init(&bus, &cfg));

    lb_node_init(&node, &bus, &platform_impl, &identity_cfg, endpoints, ENDPOINT_COUNT);
    node.on_frame = on_frame_seen;
    /* Before lb_node_start: its first state report has to reach the LED. */
    ESP_ERROR_CHECK(lb_esp_status_init(&status, &node, CONFIG_LAGUN_SERVICE_GPIO,
                                       CONFIG_LAGUN_STATUS_LED_GPIO,
                                       STATUS_LED_ACTIVE_LOW));
    lb_node_start(&node);

    lb_sense_init(&temperature, &node, &endpoints[0]);
    lb_sense_init(&humidity, &node, &endpoints[1]);
    lb_sense_init(&light, &node, &endpoints[2]);

    ESP_LOGI(TAG, "ShortId 0x%08lX, SHT40 0x%02X and VEML7700 0x%02X on SDA GPIO%d, SCL GPIO%d",
             (unsigned long)lb_node_short_id(&node), SHT4X_ADDRESS, VEML7700_ADDRESS,
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
        sht_tick(now);
        light_tick(now);
        log_tick(now);
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
