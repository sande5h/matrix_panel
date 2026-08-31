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
#include "usage.h"
#include "screen.h"
#include "server.h"

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

    char date[16], hhmm[8];
    snprintf(date, sizeof(date), "%s %02d %s",
             DAYS[tm.tm_wday % 7], tm.tm_mday, MONTHS[tm.tm_mon % 12]);
    snprintf(hhmm, sizeof(hhmm), "%02d%c%02d",
             tm.tm_hour, (tm.tm_sec & 1) ? ' ' : ':', tm.tm_min);

    hub75_clear();
    gfx_seconds_sweep(tm.tm_sec);
    gfx_text_center(4,  date, 1, 0, 140, 170);
    gfx_text_center(13, hhmm, 3, 255, 170, 40);

    /* Claude quota underneath: the five hour window, then the seven day one.
     * Blank rows until the first poll lands, rather than a misleading zero. */
    usage_t u;
    if (usage_get(&u)) {
        gfx_bar(40, 'S', u.session_pct, u.stale);
        gfx_bar(52, 'W', u.weekly_pct,  u.stale);
    }
}

/* The font covers 0x20..0x5A, so anything shown from the server -- "4h 59m"
 * and friends -- has to be folded to uppercase first. */
static void upper(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        char c = src[i];
        dst[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    dst[i] = '\0';
}

/* The quota, given the whole panel: the five hour window as a big percentage
 * because that is the one that bites, the seven day window as a bar. */
static void draw_usage(void)
{
    usage_t u;
    hub75_clear();

    if (!usage_get(&u)) {
        gfx_text_center(16, "CLAUDE", 2, 0, 140, 170);
        gfx_text_center(38, "NO DATA", 1, 120, 120, 130);
        return;
    }

    char pct[8], resets[16];
    snprintf(pct, sizeof(pct), "%d%%", u.session_pct);

    uint8_t r, g, b;
    if (u.session_pct < 60)      { r = 0;   g = 200; b = 60; }
    else if (u.session_pct < 85) { r = 255; g = 170; b = 0;  }
    else                         { r = 255; g = 40;  b = 40; }
    if (u.stale) { r /= 3; g /= 3; b /= 3; }

    gfx_text_center(1, "SESSION", 1, 0, 140, 170);
    gfx_text_center(9, pct, 3, r, g, b);

    upper(resets, sizeof(resets), u.session_resets);
    gfx_text_center(32, resets, 1, 90, 90, 110);

    gfx_bar(42, 'W', u.weekly_pct, u.stale);

    upper(resets, sizeof(resets), u.weekly_resets);
    gfx_text_center(55, resets, 1, 90, 90, 110);
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

/* Checked once per video frame so a screen change interrupts playback. */
static bool on_video_screen(void)
{
    return screen_get() == SCREEN_VIDEO;
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
    screen_set(SCREEN_VIDEO);
#endif

    ESP_ERROR_CHECK(net_start());
    usage_start();

    for (int spin = 0; !net_connected(); spin++) {
        draw_status("WIFI", NULL, spin);
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    ESP_ERROR_CHECK(server_start());
    draw_status("NET OK", net_ip(), 0);
    vTaskDelay(pdMS_TO_TICKS(1500));

    for (int spin = 0; !net_time_valid(); spin++) {
        draw_status("SYNC", NULL, spin);
        if (net_sync_time(10000) != ESP_OK) {
            ESP_LOGW(TAG, "retrying time sync");
        }
    }

    ESP_LOGI(TAG, "running, control API on http://%s:8088/", net_ip());
    int64_t next_log = 0;
    while (1) {
        switch (screen_get()) {
        case SCREEN_USAGE:
            draw_usage();
            vTaskDelay(pdMS_TO_TICKS(250));
            break;

        case SCREEN_VIDEO:
            /* Blocks until the clip ends or the screen changes under it. */
            if (!video_play(true, on_video_screen)) {
                ESP_LOGW(TAG, "no clip flashed, back to the clock");
                screen_set(SCREEN_CLOCK);
            }
            break;

        case SCREEN_CLOCK:
        default:
            draw_clock();
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }

        int64_t now = esp_timer_get_time();
        if (now >= next_log) {          /* periodic heartbeat, not change gated */
            usage_t u;
            bool have = usage_get(&u);
            ESP_LOGI(TAG, "screen %s, refresh %.0f Hz, ip %s, wifi %s, usage %s",
                     screen_name(screen_get()), hub75_refresh_hz(), net_ip(),
                     net_connected() ? "up" : "down",
                     have ? (u.stale ? "stale" : "fresh") : "waiting");
            next_log = now + 30000000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
