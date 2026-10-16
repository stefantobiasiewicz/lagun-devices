#include "ws2812.h"

#include "esp_rom_sys.h"

#define RESOLUTION_HZ 10000000u /* 0.1 µs a tick */
#define LATCH_US      300u

esp_err_t ws2812_init(ws2812_t *strip, int gpio)
{
    rmt_tx_channel_config_t channel = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RESOLUTION_HZ,
        .mem_block_symbols = 48, /* one block of the C3; the encoder refills it */
        .trans_queue_depth = 1,
    };
    esp_err_t err = rmt_new_tx_channel(&channel, &strip->channel);
    if (err != ESP_OK) {
        return err;
    }
    rmt_bytes_encoder_config_t bits = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    err = rmt_new_bytes_encoder(&bits, &strip->encoder);
    if (err != ESP_OK) {
        return err;
    }
    return rmt_enable(strip->channel);
}

esp_err_t ws2812_show(ws2812_t *strip, const uint8_t *bytes, size_t len)
{
    rmt_transmit_config_t once = {.loop_count = 0};
    esp_err_t err = rmt_transmit(strip->channel, strip->encoder, bytes, len, &once);
    if (err != ESP_OK) {
        return err;
    }
    err = rmt_tx_wait_all_done(strip->channel, 10);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(LATCH_US);
    return ESP_OK;
}
