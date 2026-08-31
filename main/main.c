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
#include "nowplaying.h"
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
    gfx_text_center(5,  date, 1, 0, 140, 170);
    gfx_text_center(19, hhmm, 3, 255, 170, 40);

    /* Whatever is playing, in the gap between the time and the quota bars.
     * Nothing playing leaves the row empty rather than reserving space for it. */
    static int np_off;
    nowplaying_t np;
    if (nowplaying_get(&np)) {
        gfx_marquee(42, np.title, 1, &np_off, 130, 130, 145);
    } else {
        np_off = 0;
    }

    /* Claude quota along the bottom: the five hour window, a four row gap,
     * then the seven day window on the last four rows. No labels and no
     * numbers -- the length is the reading. Blank until the first poll lands,
     * rather than a misleading zero. */
    usage_t u;
    if (usage_get(&u)) {
        gfx_bar_full(52, 4, u.session_pct, u.stale);
        gfx_bar_full(60, 4, u.weekly_pct,  u.stale);
    }
}

/* One quota window: its name and countdown on one line, the percentage large
 * beneath, then a tracked bar. Laid out from `top` so both windows can use it
 * and stay identical. */
static void draw_window(int top, const char *name, int pct,
                        const char *resets, bool stale)
{
    char txt[16];

    gfx_text(2, top, name, 1, 0, 140, 170);

    if (resets[0]) {
        gfx_text(HUB75_WIDTH - 2 - gfx_text_width(resets, 1), top, resets,
                 1, 90, 90, 110);
    }

    uint8_t r, g, b;
    gfx_level_color(pct, stale, &r, &g, &b);
    snprintf(txt, sizeof(txt), "%d%%", pct);
    gfx_text(2, top + 9, txt, 2, r, g, b);

    gfx_bar_track(2, top + 25, HUB75_WIDTH - 4, 6, pct, stale);
}

/* Both quota windows, stacked. The clock face says that you are consuming
 * quota; this says how much and how long until it comes back. */
static void draw_claude(void)
{
    usage_t u;
    hub75_clear();

    if (!usage_get(&u)) {
        gfx_text_center(16, "CLAUDE", 2, 0, 140, 170);
        gfx_text_center(38, "NO DATA", 1, 120, 120, 130);
        return;
    }

    /* Offsets chosen so the weekly bar ends exactly on the last row: a block
     * is 31 rows tall, and 33 + 25 + 6 = 64. */
    draw_window(0,  "SESSION", u.session_pct, u.session_resets, u.stale);
    draw_window(33, "WEEK",    u.weekly_pct,  u.weekly_resets,  u.stale);
}

/* Clamped so a bad push cannot scribble a huge number across the row, and so
 * the compiler can bound the formatted width: without a ceiling it has to
 * assume nine digits per field. 99:59 is far more than any track. */
static int clamp_secs(int s)
{
    if (s < 0) return 0;
    if (s > 99 * 60 + 59) return 99 * 60 + 59;
    return s;
}

/* Whatever the Mac last pushed. Long titles scroll; short ones sit centred.
 * The elapsed time is advanced locally between pushes so the bar moves
 * smoothly instead of stepping once a second. */
static void draw_nowplaying(void)
{
    static int title_off, artist_off;
    nowplaying_t np;

    hub75_clear();

    if (!nowplaying_get(&np)) {
        gfx_text_center(20, "NOTHING", 2, 60, 60, 75);
        gfx_text_center(38, "PLAYING", 2, 60, 60, 75);
        title_off = artist_off = 0;
        return;
    }

    const uint16_t *art = nowplaying_art();
    gfx_text_center(0, np.playing ? "NOW PLAYING" : "PAUSED", 1, 0, 140, 170);

    int dur = clamp_secs(np.duration_s);
    int pos = np.position_s;
    if (np.playing) {
        pos += (int)((esp_timer_get_time() - np.updated_us) / 1000000);
    }
    pos = clamp_secs(pos);
    if (dur && pos > dur) pos = dur;

    char times[24];
    if (dur > 0) {
        snprintf(times, sizeof(times), "%d:%02d / %d:%02d",
                 pos / 60, pos % 60, dur / 60, dur % 60);
    } else {
        times[0] = '\0';
    }

    if (art) {
        /* Artwork left, text in the column beside it. The text is drawn first
         * so the thumbnail paints over anything that scrolled under it. */
        const int col = ART_W + 6;                  /* 54 */
        const int colw = HUB75_WIDTH - col - 2;     /* 72 */

        gfx_marquee_at(col, colw, 12, np.title,  1, &title_off,  255, 170, 40);
        gfx_marquee_at(col, colw, 24, np.artist, 1, &artist_off, 120, 120, 140);

        if (dur > 0) {
            gfx_bar_track(col, 36, colw, 5, pos * 100 / dur, !np.playing);

            /* Directly under the bar, centred in the column. Past about ten
             * minutes the string outgrows the column, so it is nudged back to
             * stay inside the panel rather than being clipped. */
            int tw = gfx_text_width(times, 1);
            int tx = col + (colw - tw) / 2;
            if (tx + tw > HUB75_WIDTH - 2) tx = HUB75_WIDTH - 2 - tw;
            if (tx < col) tx = col;
            gfx_text(tx, 42, times, 1, 90, 90, 110);
        }

        gfx_blit_rgb565(2, 9, ART_W, ART_H, art);
    } else {
        gfx_marquee(10, np.title,  2, &title_off,  255, 170, 40);
        gfx_marquee(28, np.artist, 1, &artist_off, 120, 120, 140);
        if (dur > 0) {
            gfx_bar_track(2, 41, HUB75_WIDTH - 4, 6, pos * 100 / dur, !np.playing);
            gfx_text_center(50, times, 1, 90, 90, 110);
        }
    }
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
        case SCREEN_CLAUDE:
            draw_claude();
            vTaskDelay(pdMS_TO_TICKS(250));
            break;

        case SCREEN_NOWPLAYING:
            draw_nowplaying();
            vTaskDelay(pdMS_TO_TICKS(50));   /* fast enough to scroll smoothly */
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
            /* 20 Hz rather than 10: the clock itself does not need it, but the
             * track title scrolls a pixel per frame and 10 px/s reads as a
             * stutter. */
            vTaskDelay(pdMS_TO_TICKS(50));
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
