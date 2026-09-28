#include "lb_esp32c3.h"

#include "driver/twai.h"
#include "esp_log.h"

#include <inttypes.h>

static const char *TAG = "lagunbus";

/** Whether recovery after `bus-off` is under way. The controller will not take
 *  `twai_start()` meanwhile. */
static bool recovering = false;

static bool esp_send(void *ctx, const lb_frame_t *frame)
{
    (void)ctx;
    twai_message_t msg = {0};
    msg.identifier = frame->can_id;
    msg.extd = 1;                 /* LagunBus uses extended frames EXCLUSIVELY */
    msg.data_length_code = frame->len;
    for (uint8_t i = 0; i < frame->len; i++) {
        msg.data[i] = frame->data[i];
    }

    /* No waiting. A full queue is not an error — the node is to drop the frame and go
     * back to receiving, not stall on the send (transport.h). */
    return twai_transmit(&msg, 0) == ESP_OK;
}

static bool esp_recv(void *ctx, lb_frame_t *out)
{
    (void)ctx;
    twai_message_t msg;
    if (twai_receive(&msg, 0) != ESP_OK) {
        return false;
    }
    if (!msg.extd || msg.rtr) {
        /* A standard or remote frame. Not ours — a shared bus may carry other
         * people's traffic and there is no reason to take it apart. */
        return false;
    }
    out->can_id = msg.identifier;
    out->len = msg.data_length_code > LB_MAX_PAYLOAD ? LB_MAX_PAYLOAD
                                                     : msg.data_length_code;
    for (uint8_t i = 0; i < out->len; i++) {
        out->data[i] = msg.data[i];
    }
    return true;
}

static bool esp_health(void *ctx, uint16_t *tec, uint16_t *rec, uint8_t *state)
{
    (void)ctx;
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        return false;
    }
    *tec = (uint16_t)st.tx_error_counter;
    *rec = (uint16_t)st.rx_error_counter;

    switch (st.state) {
    case TWAI_STATE_RUNNING:
        /* The controller does not tell "active" from "warning", so we decide by the
         * threshold of 96 — the same one `Diagnostics.ErrorThreshold` carries. */
        *state = (st.tx_error_counter > 96 || st.rx_error_counter > 96)
               ? LB_BUS_WARNING : LB_BUS_ACTIVE;
        break;
    case TWAI_STATE_BUS_OFF:
    case TWAI_STATE_RECOVERING:
        *state = LB_BUS_OFF;
        break;
    default:
        *state = LB_BUS_PASSIVE;
        break;
    }
    return true;
}

esp_err_t lb_esp_bus_init(lb_transport_t *out, const lb_esp_bus_cfg_t *cfg)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(cfg->tx, cfg->rx,
                                                          TWAI_MODE_NORMAL);
    g.rx_queue_len = 32;
    g.tx_queue_len = 16;

    twai_timing_config_t t;
    switch (cfg->bitrate) {
    case 125000: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS(); break;
    case 250000: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS(); break;
    case 500000: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(); break;
    case 1000000: t = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS(); break;
    default:
        ESP_LOGE(TAG, "unsupported bitrate %lu", (unsigned long)cfg->bitrate);
        return ESP_ERR_INVALID_ARG;
    }

    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err != ESP_OK) {
        return err;
    }
    err = twai_start();
    if (err != ESP_OK) {
        twai_driver_uninstall();
        return err;
    }

    out->send = esp_send;
    out->recv = esp_recv;
    out->health = esp_health;
    out->ctx = NULL;

    ESP_LOGI(TAG, "TWAI on TX=%d RX=%d, %lu bit/s",
             (int)cfg->tx, (int)cfg->rx, (unsigned long)cfg->bitrate);
    return ESP_OK;
}

void lb_esp_bus_tick(lb_transport_t *bus)
{
    (void)bus;
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        return;
    }

    if (st.state == TWAI_STATE_BUS_OFF && !recovering) {
        /* A hundred and twenty-eight error-free gaps — that is what the standard
         * requires before the controller may rejoin the bus. The driver counts them
         * itself. */
        ESP_LOGW(TAG, "bus-off (tec=%" PRIu32 " rec=%" PRIu32 ") — recovering",
                 st.tx_error_counter, st.rx_error_counter);
        if (twai_initiate_recovery() == ESP_OK) {
            recovering = true;
        }
        return;
    }

    if (st.state == TWAI_STATE_STOPPED && recovering) {
        if (twai_start() == ESP_OK) {
            recovering = false;
            ESP_LOGI(TAG, "bus is back");
        }
    }
}
