#include "veml7700.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REG_ALS_CONF 0x00u
#define REG_PSM      0x03u
#define REG_ALS      0x04u

/* ALS_CONF: gain in bits 12:11, integration time in 9:6, shutdown in bit 0. */
#define GAIN_X1      0x0u
#define GAIN_X2      0x1u
#define GAIN_X1_8    0x2u
#define GAIN_X1_4    0x3u
#define IT_25        0xCu
#define IT_50        0x8u
#define IT_100       0x0u
#define IT_200       0x1u
#define IT_400       0x2u
#define IT_800       0x3u

#define TIMEOUT_MS 10

/** The window the application note keeps the count in: below it the resolution is too
 * coarse, above it the sensor nears saturation and non-linearity. */
#define COUNT_LOW  100u
#define COUNT_HIGH 10000u

typedef struct {
    uint8_t  gain;
    uint8_t  it;
    uint16_t it_ms;
    float    lux_per_count;
} rung_t;

/** Most sensitive first. The resolution is 0.0036 lx at ×2 / 800 ms and doubles with each
 * halving of gain or time (datasheet, "Resolution and maximum detection range"). */
static const rung_t LADDER[] = {
    {GAIN_X2,   IT_800, 800, 0.0036f},
    {GAIN_X2,   IT_400, 400, 0.0072f},
    {GAIN_X2,   IT_200, 200, 0.0144f},
    {GAIN_X2,   IT_100, 100, 0.0288f},
    {GAIN_X1,   IT_100, 100, 0.0576f},
    {GAIN_X1_4, IT_100, 100, 0.2304f},
    {GAIN_X1_8, IT_100, 100, 0.4608f},
    {GAIN_X1_8, IT_50,   50, 0.9216f},
    {GAIN_X1_8, IT_25,   25, 1.8432f},
};
#define RUNGS ((uint8_t)(sizeof LADDER / sizeof LADDER[0]))
/** Where a fresh chip starts: ×1 / 100 ms, ~3.7 klx full scale — a room by day. */
#define RUNG_START 4u

esp_err_t veml7700_init(veml7700_t *v, i2c_master_bus_handle_t bus, uint8_t address,
                        uint32_t scl_speed_hz)
{
    *v = (veml7700_t){.rung = RUNG_START};
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = scl_speed_hz,
    };
    return i2c_master_bus_add_device(bus, &cfg, &v->dev);
}

static esp_err_t write_reg(veml7700_t *v, uint8_t reg, uint16_t value)
{
    uint8_t frame[3] = {reg, (uint8_t)(value & 0xFFu), (uint8_t)(value >> 8)};
    return i2c_master_transmit(v->dev, frame, sizeof frame, TIMEOUT_MS);
}

static esp_err_t read_reg(veml7700_t *v, uint8_t reg, uint16_t *value)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(v->dev, &reg, 1, rx, sizeof rx, TIMEOUT_MS);
    if (err == ESP_OK) {
        *value = (uint16_t)(rx[0] | (rx[1] << 8)); /* little-endian */
    }
    return err;
}

/** Apply the current rung, powered on, no interrupt, no persistence filter. */
static esp_err_t apply(veml7700_t *v)
{
    const rung_t *r = &LADDER[v->rung];
    uint16_t conf = (uint16_t)((r->gain << 11) | (r->it << 6));
    esp_err_t err = write_reg(v, REG_ALS_CONF, conf);
    /* Two reads go: the one in progress mixes old and new, the next is whole. */
    v->settling = 2;
    return err;
}

esp_err_t veml7700_configure(veml7700_t *v)
{
    v->rung = RUNG_START;
    esp_err_t err = write_reg(v, REG_PSM, 0x0000); /* power saving off */
    if (err == ESP_OK) {
        err = apply(v);
    }
    vTaskDelay(pdMS_TO_TICKS(3)); /* ≥ 2.5 ms after power-on (datasheet) */
    return err;
}

uint32_t veml7700_integration_ms(const veml7700_t *v) { return LADDER[v->rung].it_ms; }

/** The application note's correction above ~1000 lx, where the response bends. */
static float corrected(float lux)
{
    if (lux <= 1000.0f) {
        return lux;
    }
    return ((6.0135e-13f * lux - 9.3924e-9f) * lux + 8.1488e-5f) * lux * lux + 1.0023f * lux;
}

esp_err_t veml7700_read(veml7700_t *v, uint32_t *centilux)
{
    uint16_t count = 0;
    esp_err_t err = read_reg(v, REG_ALS, &count);
    if (err != ESP_OK) {
        return err;
    }
    if (v->settling > 0) {
        v->settling--;
        return VEML7700_SETTLING;
    }
    /* The count belongs to the rung it was taken at, whatever comes next. */
    const rung_t *taken = &LADDER[v->rung];
    bool saturated = count == 0xFFFFu;
    bool moved = false;
    if ((count > COUNT_HIGH || saturated) && v->rung + 1u < RUNGS) {
        v->rung++;
        moved = true;
    } else if (count < COUNT_LOW && v->rung > 0) {
        v->rung--;
        moved = true;
    }
    if (moved && (err = apply(v)) != ESP_OK) {
        return err;
    }
    /* Saturated is no value — the light is somewhere above this range — unless there is no
     * coarser range left, and then full scale (~120 klx) is the best the chip can say. */
    if (saturated && moved) {
        return VEML7700_SETTLING;
    }
    float lux = corrected((float)count * taken->lux_per_count);
    *centilux = (uint32_t)(lux * 100.0f + 0.5f);
    return ESP_OK;
}
