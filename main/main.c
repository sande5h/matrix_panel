#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hub75.h"
#include "gfx.h"
#include "net.h"
#include "video.h"
#include "tests.h"

static const char *TAG = "main";

/* Boot-time hardware checks, off by default. PIN_CHECK holds each half in each
   primary for 5 s, long enough to meter the data pin; SELF_TEST is the shorter
   confidence check. Turn either on while wiring or after swapping boards. */
#define PIN_CHECK 0
#define SELF_TEST 0

/* Play a flashed clip instead of the clock. */
#define SHOW_VIDEO 0

static const char *const DAYS[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
static const char *const MONTHS[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                     "JUL","AUG","SEP","OCT","NOV","DEC"};

/* ---- the clock face -----------------------------------------------------
 * 128x64 in three bands: date across the top, the hours and minutes big in
 * the middle, seconds underneath. The colon blinks once a second, which is
 * also the cheapest proof the display is still being updated. */
static void draw_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char date[16], hhmm[8], secs[4];
    snprintf(date, sizeof(date), "%s %02d %s",
             DAYS[tm.tm_wday % 7], tm.tm_mday, MONTHS[tm.tm_mon % 12]);
    snprintf(hhmm, sizeof(hhmm), "%02d%c%02d",
             tm.tm_hour, (tm.tm_sec & 1) ? ' ' : ':', tm.tm_min);
    snprintf(secs, sizeof(secs), "%02d", tm.tm_sec);

    hub75_clear();
    gfx_text_center(3,  date, 1, 0,   140, 170);
    gfx_text_center(15, hhmm, 3, 255, 170, 40);
    gfx_text_center(44, secs, 2, 90,  90,  110);
}

/* Shown until the network and the clock are both up. Without this the panel
 * would sit dark through a slow DHCP and look broken. */
static void draw_status(const char *line1, const char *line2, int spin)
{
    static const char dots[][4] = {"", ".", "..", "..."};
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%s", line1, dots[spin & 3]);

    hub75_clear();
    gfx_text_center(18, buf, 2, 255, 170, 40);
    if (line2) gfx_text_center(42, line2, 1, 0, 140, 170);
}

void app_main(void)
{
    ESP_ERROR_CHECK(hub75_init());
    ESP_ERROR_CHECK(hub75_start());
    hub75_set_brightness(128);

#if PIN_CHECK
    pin_check();
#endif
#if SELF_TEST
    self_test();
#endif

#if SHOW_VIDEO
    if (video_play(true)) return;
    ESP_LOGW(TAG, "no clip flashed, falling back to the clock");
#endif

    ESP_ERROR_CHECK(net_start());

    for (int spin = 0; !net_connected(); spin++) {
        draw_status("WIFI", NULL, spin);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    draw_status("NET OK", net_ip(), 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    for (int spin = 0; !net_time_valid(); spin++) {
        draw_status("SYNC", NULL, spin);
        if (net_sync_time(10000) != ESP_OK) {
            ESP_LOGW(TAG, "retrying time sync");
        }
    }

    ESP_LOGI(TAG, "clock running");
    int64_t next_log = 0;
    while (1) {
        draw_clock();

        int64_t now = esp_timer_get_time();
        if (now >= next_log) {          /* periodic heartbeat, not change gated */
            ESP_LOGI(TAG, "refresh %.0f Hz, ip %s, wifi %s",
                     hub75_refresh_hz(), net_ip(),
                     net_connected() ? "up" : "down");
            next_log = now + 30000000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
