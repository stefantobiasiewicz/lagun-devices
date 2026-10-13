#include "lb_esp32c3.h"

#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <string.h>

#define NVS_NAMESPACE "lagunbus"

static uint32_t esp_millis(void *ctx)
{
    (void)ctx;
    /* `esp_timer_get_time()` counts microseconds and does not step backwards when the
     * clock is set — which is what transaction deadlines require (platform.h). It wraps
     * after 49 days; subtraction on uint32_t goes through that correctly,
     * as long as we never compare timestamps directly. */
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool esp_store(void *ctx, const char *key, const void *data, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static bool esp_load(void *ctx, const char *key, void *out, size_t len)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;  /* the namespace does not exist yet — first boot */
    }
    size_t how_many = len;
    esp_err_t err = nvs_get_blob(h, key, out, &how_many);
    nvs_close(h);
    return err == ESP_OK && how_many == len;
}

/** Forget a key — the factory reset erases every stored setting (lb_platform_t.erase).
 * A key that was never stored answers ESP_ERR_NVS_NOT_FOUND, which is not a fault. */
static void esp_erase(void *ctx, const char *key)
{
    (void)ctx;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_erase_key(h, key) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static uint32_t esp_rand(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static void esp_restart_node(void *ctx)
{
    (void)ctx;
    /* The acknowledgement of FactoryReset left the TWAI queue during the grace period
     * the library waited (LB_RESET_GRACE_MS); what is still in RAM goes with the
     * restart, which is the point. */
    esp_restart();
}

/** `LastResetReason` from what the chip knows (types/0x0001-device.md §2.4).
 *
 * A panic goes in as WATCHDOG: the specification has two values that mean a fault —
 * WATCHDOG for "the firmware hung" and BROWNOUT for the supply — and a crash is the
 * firmware's fault as much as a hang is; UNKNOWN would hide it. Until 2026-10-03 the node
 * did not ask and always said POWER_ON (zadania/zrobione/CR-05.md). */
static uint8_t esp_reset_reason_node(void *ctx)
{
    (void)ctx;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return LB_RESET_POWER_ON;
    case ESP_RST_EXT:
    case ESP_RST_SDIO:     return LB_RESET_EXTERNAL;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
    case ESP_RST_USB:      /* the USB Serial/JTAG reset — flashing, monitor.sh */
    case ESP_RST_JTAG:     return LB_RESET_EXTERNAL;
#endif
    case ESP_RST_SW:       return LB_RESET_SOFTWARE;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return LB_RESET_WATCHDOG;
    case ESP_RST_BROWNOUT: return LB_RESET_BROWNOUT;
    default:               return LB_RESET_UNKNOWN;
    }
}

esp_err_t lb_esp_platform_init(lb_platform_t *out)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Storage left over from a changed partition layout or newer firmware. We
         * erase it — the result is a node that announces itself anew, rather than one
         * that does not come up at all. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    out->millis = esp_millis;
    out->store = esp_store;
    out->load = esp_load;
    out->erase = esp_erase;
    out->random = esp_rand;
    out->restart = esp_restart_node;
    out->reset_reason = esp_reset_reason_node;
    out->ctx = NULL;
    return ESP_OK;
}

esp_err_t lb_esp_uid(uint8_t uid[8])
{
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY);
    if (err != ESP_OK) {
        return err;
    }

    /* MAC REVERSED — see the comment at the declaration. The reason used to be that
     * `ShortId` took the four high bytes of Uid, while the first three bytes of a MAC
     * are Espressif's OUI, identical across all boards — without the reversal every one
     * of our nodes would have had almost the same ShortId.
     *
     * That reason is gone: `lb_node_short_id()` computes CRC-32 over the whole Uid, so
     * the byte layout no longer matters. The reversal stays regardless, because
     * CHANGING it now would change the Uid of every node already flashed — and a Uid is
     * an identity for the life of the device, so the hub would see them as entirely new
     * ones. */
    for (int i = 0; i < 6; i++) {
        uid[i] = mac[5 - i];
    }
    uid[6] = 0x00;
    uid[7] = 0x01;  /* Uid layout marker — in case it ever has to change */
    return ESP_OK;
}
