#include "lb_esp32c3.h"

#include "esp_log.h"

static const char *TAG = "status";

/** 15 ms, as the keys: past a tact switch's bounce, far below a 5 s hold. */
#define SERVICE_DEBOUNCE_MS 15u

static bool button_pressed(const lb_esp_status_t *s)
{
    /* To ground, with the pull-up: LOW is a finger on it. BOOT on the Super Mini is
     * wired exactly so. */
    return gpio_get_level(s->button_gpio) == 0;
}

static void drive_led(void *ctx, bool on)
{
    lb_esp_status_t *s = ctx;
    gpio_set_level(s->led_gpio, (on != s->led_active_low) ? 1 : 0);
}

/** The node's state watch: on to the LED, and into the log — the state changes rarely,
 * so this is the one line in the log that says what the node is doing. */
static void on_state(void *ctx, lb_node_state_t state)
{
    lb_esp_status_t *s = ctx;
    ESP_LOGI(TAG, "%s", lb_node_state_name(state));
    lb_led_on_state(&s->led, state);
}

esp_err_t lb_esp_status_init(lb_esp_status_t *s, lb_node_t *node, gpio_num_t button_gpio,
                             gpio_num_t led_gpio, bool led_active_low)
{
    *s = (lb_esp_status_t){
        .node = node, .button_gpio = button_gpio, .led_gpio = led_gpio,
        .led_active_low = led_active_low,
    };
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << button_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&in);
    if (err != ESP_OK) {
        return err;
    }
    /* The LED's level is set BEFORE the pin becomes an output, dark — GPIO8 is a
     * strapping pin, and an LED from 3V3 keeps it high, which is what boot wants. */
    gpio_set_level(led_gpio, led_active_low ? 1 : 0);
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << led_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&out);
    if (err != ESP_OK) {
        return err;
    }

    lb_led_init(&s->led, drive_led, s);
    lb_service_init(&s->service, LB_SERVICE_HOLD_MS);
    lb_debounce_init(&s->debounce, button_pressed(s), 0);
    node->on_state = on_state;
    node->state_ctx = s;
    return ESP_OK;
}

void lb_esp_status_tick(lb_esp_status_t *s, uint32_t now_ms)
{
    bool pressed = lb_debounce_update(&s->debounce, button_pressed(s), now_ms,
                                      SERVICE_DEBOUNCE_MS);
    lb_service_event_t event = lb_service_update(&s->service, pressed, now_ms);
    if (event == LB_SERVICE_HELD && !s->led.release) {
        ESP_LOGW(TAG, "service button held %u s — let go to reset",
                 (unsigned)(LB_SERVICE_HOLD_MS / 1000u));
    }
    lb_led_release(&s->led, event == LB_SERVICE_HELD);
    if (event == LB_SERVICE_RESET) {
        ESP_LOGW(TAG, "service button: factory reset");
        lb_node_factory_reset(s->node);
    }
    lb_led_tick(&s->led, now_ms);
}
