#include "sht4x.h"

#define CMD_MEASURE_HIGH 0xFDu
#define CMD_SOFT_RESET   0x94u

/** A command is one byte, a reading six; 10 ms is plenty and keeps a dead chip from
 * holding the node's loop (see mcp23017.c). */
#define TIMEOUT_MS 10

esp_err_t sht4x_init(sht4x_t *s, i2c_master_bus_handle_t bus, uint8_t address,
                     uint32_t scl_speed_hz)
{
    *s = (sht4x_t){0};
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };
    return i2c_master_bus_add_device(bus, &cfg, &s->dev);
}

static esp_err_t command(sht4x_t *s, uint8_t cmd)
{
    return i2c_master_transmit(s->dev, &cmd, 1, TIMEOUT_MS);
}

esp_err_t sht4x_start(sht4x_t *s) { return command(s, CMD_MEASURE_HIGH); }

esp_err_t sht4x_reset(sht4x_t *s) { return command(s, CMD_SOFT_RESET); }

uint8_t sht4x_crc(const uint8_t *data, int len)
{
    uint8_t crc = 0xFFu;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

esp_err_t sht4x_fetch(sht4x_t *s, sht4x_reading_t *out)
{
    uint8_t rx[6];
    esp_err_t err = i2c_master_receive(s->dev, rx, sizeof rx, TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (sht4x_crc(rx, 2) != rx[2] || sht4x_crc(rx + 3, 2) != rx[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    int64_t t_raw = (int64_t)((rx[0] << 8) | rx[1]);
    int64_t rh_raw = (int64_t)((rx[3] << 8) | rx[4]);
    /* T = −45 + 175·raw/65535 °C, RH = −6 + 125·raw/65535 % — in hundredths and integers,
     * the units the bus carries; no float on the way. Rounded to the nearest. */
    out->temperature = (int32_t)(-4500 + (17500 * t_raw + 32767) / 65535);
    int32_t rh = (int32_t)(-600 + (12500 * rh_raw + 32767) / 65535);
    /* The formula reaches below 0 and above 100 %; the datasheet says to cut it. */
    out->humidity = rh < 0 ? 0 : rh > 10000 ? 10000 : rh;
    return ESP_OK;
}
