#include "lb_esp32c3.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>

/* Why the log goes through a queue (zadania/BUG-02.md).
 *
 * The console is USB Serial/JTAG, and a write to it WAITS when its 64-byte FIFO is full:
 * up to 50 ms when nobody reads the port, longer behind a slow reader. The node loop
 * logs on the way to its answer — the frame received, the output switched, the frames
 * sent — so a single command could keep the RESPONSE waiting past the hub's 100 ms, and
 * the hub then repeats the command (04-interakcje.md §2.1). Seen on `out8`: a response
 * after 108 ms, and a Toggle carried out twice.
 *
 * So ESP_LOGx only formats the line and puts it in a ring buffer, never waiting; a task
 * of its own writes the buffer out and is the only one that ever waits on USB. A full
 * buffer drops the line and counts it — a log with a gap says so, a late answer on the
 * bus would not.
 *
 * Rejected: logging less. The frame log is the one window onto a first bring-up
 * (apps/key/main.c), and any line can wait, not only many. Rejected too: the USB
 * Serial/JTAG driver — its console writes wait without a limit when nobody reads. */

#define LOG_BUFFER_BYTES 4096
#define LOG_LINE_BYTES   256

static RingbufHandle_t log_buffer;
static volatile uint32_t dropped;

static int log_to_buffer(const char *format, va_list args)
{
    char line[LOG_LINE_BYTES];
    int length = vsnprintf(line, sizeof line, format, args);
    if (length <= 0) {
        return length;
    }
    size_t size = (size_t)length < sizeof line ? (size_t)length : sizeof line - 1;
    if (xRingbufferSend(log_buffer, line, size, 0) != pdTRUE) {
        dropped++;
    }
    return length;
}

static void log_writer(void *arg)
{
    (void)arg;
    uint32_t reported = 0;
    while (true) {
        size_t size = 0;
        char *item = xRingbufferReceive(log_buffer, &size, portMAX_DELAY);
        if (item != NULL) {
            fwrite(item, 1, size, stdout);
            vRingbufferReturnItem(log_buffer, item);
        }
        uint32_t now_dropped = dropped;
        if (now_dropped != reported) {
            printf("[log: %lu lines dropped]\n", (unsigned long)(now_dropped - reported));
            reported = now_dropped;
        }
        fflush(stdout);
    }
}

esp_err_t lb_esp_log_init(void)
{
    log_buffer = xRingbufferCreate(LOG_BUFFER_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (log_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* The node loop's own priority (app_main, 1): when the writer waits on USB, the
     * scheduler still gives the loop its turn every tick (1 ms at FREERTOS_HZ=1000). */
    if (xTaskCreate(log_writer, "log", 3072, NULL, 1, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    esp_log_set_vprintf(log_to_buffer);
    return ESP_OK;
}
