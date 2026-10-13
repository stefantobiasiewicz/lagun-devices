#include "epd1in9.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "epd1in9";

/** One byte a transfer, ~0.2 ms at 100 kHz; a refresh is ~30 of them. 10 ms, as for the
 * MCP23017: the transfers run inside the node's loop, and a panel that does not answer
 * must not hold the node's answers for long. */
#define TIMEOUT_MS 10

/** BUSY low for longer than any waveform takes (1.5 s at most, 1 s frames in the cold)
 * means it is not wired, or the controller hung: the refresh is given up and the
 * controller put to sleep, so the next one starts clean. */
#define BUSY_GIVE_UP_MS 5000u
/** BUSY never seen low: the refresh is taken as done after this — the longest waveform.
 * Without it a BUSY line stuck HIGH would cut every refresh short. */
#define BUSY_UNSEEN_MS 2000u

/* --- characters to segments ------------------------------------------------------ */

/** A digit place is two bytes. From the community driver (header); the first byte of the
 * pair holds the left and middle segments, the second the right ones in its low five
 * bits — bit 0x20 of the second byte is the point or `%` where a place has one.
 * Waveshare's own tables write "1" as `1f 00` and set bit 0x20 of byte 12 for some
 * digits; the community driver does neither, and the test pattern showed it right
 * (DEVICE-04, 2026-10-13). Bit 0x20 of byte 12 stays off. */
static const uint8_t DIGIT[10][2] = {
    {0xbf, 0x1f}, {0x00, 0x1f}, {0xfd, 0x17}, {0xf5, 0x1f}, {0x47, 0x1f},
    {0xf7, 0x1d}, {0xff, 0x1d}, {0x21, 0x1f}, {0xff, 0x1f}, {0xf7, 0x1f},
};
static const uint8_t MINUS[2] = {0x44, 0x00};

#define POINT   0x20u
#define CELSIUS 0x05u

static void place(uint8_t *pair, char c)
{
    if (c >= '0' && c <= '9') {
        pair[0] = DIGIT[c - '0'][0];
        pair[1] = DIGIT[c - '0'][1];
    } else if (c == '-') {
        pair[0] = MINUS[0];
        pair[1] = MINUS[1];
    } else {
        pair[0] = 0;
        pair[1] = 0;
    }
}

void epd1in9_draw(uint8_t image[EPD1IN9_IMAGE], const char *temperature,
                  const char *humidity)
{
    memset(image, 0, EPD1IN9_IMAGE);
    /* "188.8": [0] the half digit, [1] tens, [2] units, [3] the point, [4] tenths. */
    if (temperature != NULL && strlen(temperature) == EPD1IN9_TEMPERATURE_PLACES) {
        image[0] = temperature[0] == '1' ? 0xff : 0x00;
        place(&image[1], temperature[1]);
        place(&image[3], temperature[2]);
        place(&image[11], temperature[4]);
        image[4] |= POINT;
        image[13] = CELSIUS; /* N4: always °C */
    }
    /* "88.8": [0] tens, [1] units, [2] the point, [3] tenths, then %. */
    if (humidity != NULL && strlen(humidity) == EPD1IN9_HUMIDITY_PLACES) {
        place(&image[5], humidity[0]);
        place(&image[7], humidity[1]);
        place(&image[9], humidity[3]);
        image[8] |= POINT;
        image[10] |= POINT; /* % */
    }
}

/* --- the controller -------------------------------------------------------------- */

static esp_err_t command(epd1in9_t *panel, uint8_t byte)
{
    return i2c_master_transmit(panel->command, &byte, 1, TIMEOUT_MS);
}

static esp_err_t commands(epd1in9_t *panel, const uint8_t *bytes, size_t count)
{
    /* One byte a transfer, as the demo does: whether the controller takes several in one
     * is not documented. */
    for (size_t i = 0; i < count; i++) {
        esp_err_t err = command(panel, bytes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t data(epd1in9_t *panel, uint8_t byte)
{
    return i2c_master_transmit(panel->data, &byte, 1, TIMEOUT_MS);
}

static void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

esp_err_t epd1in9_init(epd1in9_t *panel, i2c_master_bus_handle_t bus, gpio_num_t rst,
                       gpio_num_t busy, uint32_t scl_speed_hz)
{
    *panel = (epd1in9_t){.busy = busy};

    /* BUSY is pulled DOWN: an unwired line then reads "busy", every refresh ends in the
     * give-up warning, and the log says why — pulled up it would read "ready" and the
     * refreshes would silently be cut short. */
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << busy,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << rst,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&in);
    if (err == ESP_OK) {
        err = gpio_config(&out);
    }
    if (err != ESP_OK) {
        return err;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EPD1IN9_ADDRESS_COMMAND,
        .scl_speed_hz = scl_speed_hz,
    };
    err = i2c_master_bus_add_device(bus, &cfg, &panel->command);
    if (err != ESP_OK) {
        return err;
    }
    cfg.device_address = EPD1IN9_ADDRESS_DATA;
    err = i2c_master_bus_add_device(bus, &cfg, &panel->data);
    if (err != ESP_OK) {
        return err;
    }

    /* The demo's reset and power-up, with its pauses. */
    gpio_set_level(rst, 1);
    sleep_ms(200);
    gpio_set_level(rst, 0);
    sleep_ms(20);
    gpio_set_level(rst, 1);
    sleep_ms(200);
    sleep_ms(100);

    err = command(panel, 0x2B); /* power on */
    sleep_ms(10);
    if (err == ESP_OK) {
        static const uint8_t boost[] = {0xA7, 0xE0}; /* boost, TSON */
        err = commands(panel, boost, sizeof boost);
    }
    sleep_ms(10);
    if (err == ESP_OK) {
        /* The demo's "temperature" setting and the frame time for ≥ 20 °C: 300 ms. Fixed
         * for a room (N11) — the temperature the board shows is another room's, and says
         * nothing about this panel. */
        static const uint8_t drive[] = {0x7E, 0x81, 0xB4};
        err = commands(panel, drive, sizeof drive);
    }
    sleep_ms(10);
    if (err == ESP_OK) {
        static const uint8_t frame[] = {0xE7, 0x0E}; /* (14 + 1) × 20 ms */
        err = commands(panel, frame, sizeof frame);
    }
    return err;
}

esp_err_t epd1in9_refresh(epd1in9_t *panel, const uint8_t image[EPD1IN9_IMAGE],
                          epd1in9_wave_t wave, uint32_t now_ms)
{
    static const uint8_t WAVES[3][7] = {
        [EPD1IN9_WAVE_BOOT] = {0x82, 0x28, 0x20, 0xA8, 0xA0, 0x50, 0x65},
        [EPD1IN9_WAVE_FULL] = {0x82, 0x20, 0x00, 0xA0, 0x80, 0x40, 0x63},
        [EPD1IN9_WAVE_QUICK] = {0x82, 0x80, 0x00, 0xC0, 0x80, 0x80, 0x62},
    };
    /* Wake, power on, RAM from address 0, the first SRAM on and off. */
    static const uint8_t before[] = {0xAC, 0x2B, 0x40, 0xA9, 0xA8};
    /* The second SRAM on and off, display on — the refresh starts here. */
    static const uint8_t after[] = {0xAB, 0xAA, 0xAF};

    if (panel->refreshing) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The waveform every time: the demo sets it once, but whether it survives the deep
     * sleep between refreshes is not documented — the community driver sends it again. */
    esp_err_t err = commands(panel, WAVES[wave], sizeof WAVES[wave]);
    if (err == ESP_OK) {
        err = commands(panel, before, sizeof before);
    }
    for (size_t i = 0; err == ESP_OK && i < EPD1IN9_IMAGE; i++) {
        err = data(panel, image[i]);
    }
    if (err == ESP_OK) {
        err = data(panel, 0x00); /* the demo's 16th byte */
    }
    if (err == ESP_OK) {
        err = commands(panel, after, sizeof after);
    }
    if (err == ESP_OK) {
        panel->refreshing = true;
        panel->seen_busy = false;
        panel->started_ms = now_ms;
    }
    return err;
}

/** Display off, high voltage off, deep sleep — the end of the demo's `Write_Screen`. */
static void finish(epd1in9_t *panel)
{
    static const uint8_t sleep[] = {0xAE, 0x28, 0xAD};
    esp_err_t err = commands(panel, sleep, sizeof sleep);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sleep after the refresh: %s", esp_err_to_name(err));
    }
    panel->refreshing = false;
}

void epd1in9_tick(epd1in9_t *panel, uint32_t now_ms)
{
    if (!panel->refreshing) {
        return;
    }
    uint32_t elapsed = now_ms - panel->started_ms;
    bool ready = gpio_get_level(panel->busy) != 0; /* BUSY_N: low = busy */
    if (!ready) {
        panel->seen_busy = true;
        if (elapsed >= BUSY_GIVE_UP_MS) {
            ESP_LOGW(TAG, "BUSY low for %lu ms — is it wired to GPIO%d? Giving up.",
                     (unsigned long)elapsed, (int)panel->busy);
            finish(panel);
        }
        return;
    }
    if (panel->seen_busy) {
        finish(panel);
    } else if (elapsed >= BUSY_UNSEEN_MS) {
        ESP_LOGW(TAG, "BUSY never went low during a refresh — stuck high?");
        finish(panel);
    }
}

bool epd1in9_busy(const epd1in9_t *panel) { return panel->refreshing; }
