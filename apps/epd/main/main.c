/* Epd — ESP32-C3 Super Mini + CAN transceiver + Waveshare 1.9inch Segment e-Paper.
 *
 * Two `Display` fields, VALUE only: a temperature on ep4 ("188.8 °C"), a humidity on
 * ep5 ("88.8 %"). What a field shows and when it expires is the library's
 * (`lagunbus/types/display.h`); the number in its places (`lagunbus/tools/digits.h`) and
 * when the glass may change (`lagunbus/tools/epaper.h`) are the library's tools — all
 * tested on a computer, and the same device runs there as `lagun-sim epd`. What is here
 * is the panel: drawing the picture and refreshing it without making the node wait.
 *
 * The decisions (zadania/zrobione/DEVICE-04.md):
 *
 *   - −9.9 … 199.9 °C, 0.0 … 99.9 %; outside, "absent" or expired — dashes (N1);
 *   - BUSY on GPIO20 (N2);
 *   - a refresh at most every LAGUN_EPD_MIN_GAP_S, except while the glass shows no
 *     number; a full one after LAGUN_EPD_FULL_EVERY partial ones (N3);
 *   - always °C, no icons (N4, N5); `ContentTimeout` exposed, 600 s (N6);
 *   - `DeviceType` 0x0023, class CONTROL (N8);
 *   - the panel never makes the loop wait: BUSY is looked at, not waited on (N10);
 *   - the start's refresh is the boot waveform — it clears what the glass kept from
 *     before the power went (N11; zadania/zrobione/FEAT-12.md, N13).
 */
#include "lagunbus/types/display.h"
#include "lagunbus/tools/digits.h"
#include "lagunbus/tools/epaper.h"
#include "lagunbus/registry.h"
#include "lb_esp32c3.h"
#include "epd1in9.h"

#include "esp_log.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "epd";

/** A refresh that failed on I²C is tried again after this — not at every turn of the
 * loop, which would fill the log while the glass shows no number (no gap then). */
#define RETRY_MS 5000u

/* --- the endpoints ------------------------------------------------------------------- */

#define FIELD_ROWS(quantity)                                    \
    {                                                           \
        LB_DSP_ATTR_CONTENT(),                                  \
        LB_DSP_ATTR_QUANTITY(quantity),                         \
        LB_DSP_ATTR_VALUE(),                                    \
        LB_DSP_ATTR_CONTENT_TIMEOUT(LB_DSP_TIMEOUT_DEFAULT_S),  \
    }
static lb_attr_t temperature_rows[] = FIELD_ROWS(LB_QUANTITY_TEMPERATURE);
static lb_attr_t humidity_rows[] = FIELD_ROWS(LB_QUANTITY_HUMIDITY);

static lb_display_t temperature, humidity;

static uint8_t on_write(lb_endpoint_t *self, uint8_t element, lb_value_t value)
{
    return lb_display_write(self->ctx, element, value);
}

/* Channel 0: a field belongs to no channel (types/0x0022-display.md). */
#define FIELD_ENDPOINT(n, rows, field)                    \
    {                                                     \
        .number = (n),                                    \
        .type_id = LB_TYPE_DISPLAY,                       \
        .revision = 1,                                    \
        .features = LB_DSP_F_VALUE,                       \
        .channel = 0,                                     \
        .attrs = (rows),                                  \
        .attr_count = sizeof(rows) / sizeof((rows)[0]),   \
        .on_check = lb_display_check,                     \
        .feature_of = lb_display_feature_of,              \
        .on_write = on_write,                             \
        .ctx = (field),                                   \
    }
static lb_endpoint_t endpoints[] = {
    FIELD_ENDPOINT(4, temperature_rows, &temperature),
    FIELD_ENDPOINT(5, humidity_rows, &humidity),
};
#define ENDPOINT_COUNT (sizeof endpoints / sizeof endpoints[0])

/* The places on the glass (N1) — the same layouts as the simulator's `epd`. */
static const lb_digits_t DEGREES = {.whole = 2, .leading_one = true, .decimals = 1,
                                    .minus = true};
static const lb_digits_t PERCENT = {.whole = 2, .leading_one = false, .decimals = 1,
                                    .minus = false};

/* --- the panel ----------------------------------------------------------------------- */

static epd1in9_t   panel;
static lb_epaper_t pacer;
static uint8_t     glass[EPD1IN9_IMAGE]; /**< what the glass shows, as last refreshed */
static bool        glass_known;          /**< false before the first refresh and after a failed one */
static uint32_t    retry_ms;

static void panel_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,  /* any free controller — the C3 has one */
        .sda_io_num = CONFIG_LAGUN_I2C_SDA,
        .scl_io_num = CONFIG_LAGUN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* The module's own pull-ups do the real work; the internal ones (~45 kΩ) only
         * keep the lines high if those are missing. */
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));
    /* 100 kHz: ~30 one-byte transfers a refresh, a few ms — and a module on jumper wires
     * is happier slower. Not fatal: a panel that does not answer is retried by the loop,
     * and the node works without it. */
    esp_err_t err = epd1in9_init(&panel, i2c_bus, CONFIG_LAGUN_EPD_RST_GPIO,
                                 CONFIG_LAGUN_EPD_BUSY_GPIO, 100000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "e-paper init: %s", esp_err_to_name(err));
    }
}

/** What one field would show now, in its places; true when that is a number. */
static bool field_places(const lb_display_t *field, const lb_digits_t *layout, char *out)
{
    lb_value_t value = lb_display_content(field) == LB_DSP_SHOWS_VALUE
                           ? lb_display_value(field)
                           : LB_ABSENT;
    /* 2: hundredths — the scale of TEMPERATURE and HUMIDITY (lagunbus/values.h). */
    return lb_digits_format(layout, value, 2, out);
}

/** The loop's part of the screen: compose the picture, ask the pacer, refresh. */
static void screen_tick(uint32_t now)
{
    epd1in9_tick(&panel, now);
    if (epd1in9_busy(&panel) || (int32_t)(now - retry_ms) < 0) {
        return;
    }

    char t[LB_DIGITS_MAX + 1], h[LB_DIGITS_MAX + 1];
    bool content = field_places(&temperature, &DEGREES, t);
    content |= field_places(&humidity, &PERCENT, h);
    uint8_t image[EPD1IN9_IMAGE];
    epd1in9_draw(image, t, h);

    bool differs = !glass_known || memcmp(image, glass, sizeof image) != 0;
    lb_epaper_kind_t kind = lb_epaper_due(&pacer, now, differs);
    if (kind == LB_EPAPER_NONE) {
        return;
    }
    epd1in9_wave_t wave = !pacer.started         ? EPD1IN9_WAVE_BOOT
                          : kind == LB_EPAPER_FULL ? EPD1IN9_WAVE_FULL
                                                   : EPD1IN9_WAVE_QUICK;
    esp_err_t err = epd1in9_refresh(&panel, image, wave, now);
    /* Counted even when it failed: the pacer's gap then also spaces the retries of a
     * panel that answers again. */
    lb_epaper_started(&pacer, now, kind, content);
    if (err == ESP_OK) {
        memcpy(glass, image, sizeof glass);
        glass_known = true;
        ESP_LOGI(TAG, "%s refresh: \"%s °C\"  \"%s %%\"",
                 wave == EPD1IN9_WAVE_BOOT ? "boot" : wave == EPD1IN9_WAVE_FULL ? "full" : "partial",
                 t, h);
    } else {
        glass_known = false;
        retry_ms = now + RETRY_MS;
        ESP_LOGW(TAG, "refresh: %s — again in %u s", esp_err_to_name(err), RETRY_MS / 1000u);
    }
}

/* --- the test pattern ---------------------------------------------------------------- */

#ifdef CONFIG_LAGUN_EPD_TEST_PATTERN

#define PATTERN_STEP_MS 4000u

/** Step `n` of the pattern into `image`, with what it should show in `about`. Returns
 * false past the last step. */
static bool pattern(unsigned n, uint8_t image[EPD1IN9_IMAGE], char *about, size_t cap)
{
    memset(image, 0, EPD1IN9_IMAGE);
    if (n == 0) {
        memset(image, 0xff, EPD1IN9_IMAGE);
        snprintf(about, cap, "everything black — the whole layout");
        return true;
    }
    n -= 1;
    if (n < EPD1IN9_IMAGE) {
        image[n] = 0xff;
        snprintf(about, cap, "byte %u alone = 0xFF", n);
        return true;
    }
    n -= EPD1IN9_IMAGE;
    if (n < 16) {
        unsigned byte = n < 8 ? 12 : 13;
        image[byte] = (uint8_t)(1u << (n % 8));
        snprintf(about, cap, "byte %u, bit 0x%02X alone", byte, 1u << (n % 8));
        return true;
    }
    n -= 16;
    if (n < 10) {
        char t[] = " 00.0", h[] = "00.0";
        t[1] = t[2] = t[4] = (char)('0' + n);
        h[0] = h[1] = h[3] = (char)('0' + n);
        epd1in9_draw(image, t, h);
        snprintf(about, cap, "digit %u in every place: \"%s °C\" \"%s %%\"", n, t, h);
        return true;
    }
    n -= 10;
    static const char *const SAMPLES[][2] = {
        {"1  .0", " 0.0"},  /* the half digit alone */
        {" -9.9", "45.5"},
        {"199.9", "99.9"},
        {" --.-", "--.-"},  /* dashes — "absent" */
        {" 21.4", "45.5"},
    };
    if (n < sizeof SAMPLES / sizeof SAMPLES[0]) {
        epd1in9_draw(image, SAMPLES[n][0], SAMPLES[n][1]);
        snprintf(about, cap, "\"%s °C\"  \"%s %%\"", SAMPLES[n][0], SAMPLES[n][1]);
        return true;
    }
    return false;
}

/** No node, no bus: the patterns, one every few seconds, over and over. A full refresh
 * after every eight partial ones, as the glass would need anyway. */
static void run_test_pattern(void)
{
    ESP_LOGW(TAG, "TEST PATTERN (LAGUN_EPD_TEST_PATTERN) — no node; compare the glass "
                  "with each line");
    uint8_t image[EPD1IN9_IMAGE];
    char about[96];
    unsigned refreshes = 0;
    for (unsigned round = 1;; round++) {
        for (unsigned n = 0; pattern(n, image, about, sizeof about); n++) {
            uint32_t now = (uint32_t)(esp_log_timestamp());
            epd1in9_wave_t wave = refreshes == 0         ? EPD1IN9_WAVE_BOOT
                                  : refreshes % 9 == 0   ? EPD1IN9_WAVE_FULL
                                                         : EPD1IN9_WAVE_QUICK;
            esp_err_t err = epd1in9_refresh(&panel, image, wave, now);
            refreshes++;
            ESP_LOGI(TAG, "[%u.%02u] %s%s", round, n, about,
                     err == ESP_OK ? "" : " — REFRESH FAILED");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "  %s", esp_err_to_name(err));
            }
            /* The step's time, and longer while the panel is still busy — its give-up
             * (5 s) ends that, so an unwired BUSY slows the pattern, not stops it. */
            for (uint32_t waited = 0; waited < PATTERN_STEP_MS || epd1in9_busy(&panel);
                 waited += 10) {
                epd1in9_tick(&panel, (uint32_t)esp_log_timestamp());
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

#endif /* CONFIG_LAGUN_EPD_TEST_PATTERN */

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

    /* Before the node: ~550 ms of reset and power-up, and nothing on the bus waits yet. */
    panel_init();
#ifdef CONFIG_LAGUN_EPD_TEST_PATTERN
    run_test_pattern();
#endif

    lb_identity_t identity_cfg = {
        .node_class = LB_CLASS_CONTROL,
        .device_type = 0x0023,  /* epd — devices/README.md § Models */
        .vendor_id = 0x0000,
        .firmware = 0x00000100,
        .hardware = 1,
        .protocol = 1,
    };
    ESP_ERROR_CHECK(lb_esp_uid(identity_cfg.uid));

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

    /* No `show`: the loop composes the picture every turn and compares it with the glass,
     * so there is nothing to be told — and the start's clear is the pacer's first, boot
     * refresh (N11). The own expiry is unused: both fields expose `ContentTimeout`. */
    lb_display_init(&temperature, &node, &endpoints[0], LB_DSP_TIMEOUT_DEFAULT_S, NULL, NULL);
    lb_display_init(&humidity, &node, &endpoints[1], LB_DSP_TIMEOUT_DEFAULT_S, NULL, NULL);
    lb_epaper_init(&pacer, CONFIG_LAGUN_EPD_MIN_GAP_S * 1000u,
                   (uint8_t)CONFIG_LAGUN_EPD_FULL_EVERY);

    ESP_LOGI(TAG, "ShortId 0x%08lX, e-paper 0x%02X/0x%02X on SDA GPIO%d, SCL GPIO%d, "
                  "RST GPIO%d, BUSY GPIO%d; refresh at most every %d s",
             (unsigned long)lb_node_short_id(&node), EPD1IN9_ADDRESS_COMMAND,
             EPD1IN9_ADDRESS_DATA, CONFIG_LAGUN_I2C_SDA, CONFIG_LAGUN_I2C_SCL,
             CONFIG_LAGUN_EPD_RST_GPIO, CONFIG_LAGUN_EPD_BUSY_GPIO,
             CONFIG_LAGUN_EPD_MIN_GAP_S);
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
        lb_display_tick(&temperature, now);
        lb_display_tick(&humidity, now);
        screen_tick(now);
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
