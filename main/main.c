#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hub75.h"

static const char *TAG = "main";

/* 6 bit hue -> RGB, no saturation/value control, just enough for a test pattern. */
static void hue_rgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t seg = hue / 43;
    uint8_t off = (uint8_t)((hue - seg * 43) * 6);
    switch (seg) {
        case 0:  *r = 255;       *g = off;       *b = 0;         break;
        case 1:  *r = 255 - off; *g = 255;       *b = 0;         break;
        case 2:  *r = 0;         *g = 255;       *b = off;       break;
        case 3:  *r = 0;         *g = 255 - off; *b = 255;       break;
        case 4:  *r = off;       *g = 0;         *b = 255;       break;
        default: *r = 255;       *g = 0;         *b = 255 - off; break;
    }
}

/* A plasma field -- exercises every colour bit on every pixel, so dead lines,
 * swapped address bits and ghosting all show up immediately. */
static void draw_plasma(float t)
{
    for (int y = 0; y < HUB75_HEIGHT; y++) {
        for (int x = 0; x < HUB75_WIDTH; x++) {
            float v = sinf(x * 0.18f + t)
                    + sinf(y * 0.14f - t * 0.8f)
                    + sinf((x + y) * 0.11f + t * 0.5f)
                    + sinf(sqrtf((float)((x - HUB75_WIDTH / 2) * (x - HUB75_WIDTH / 2) +
                                  (y - HUB75_HEIGHT / 2) * (y - HUB75_HEIGHT / 2))) * 0.22f - t);
            uint8_t hue = (uint8_t)((v + 4.0f) * (255.0f / 8.0f));
            uint8_t r, g, b;
            hue_rgb(hue, &r, &g, &b);
            hub75_set_pixel(x, y, r, g, b);
        }
    }
}

typedef void (*pattern_fn)(void);

/* Periodic heartbeat while a pattern is on screen: if frames stops climbing,
 * the DMA has stalled and nothing on the panel means anything. */
static void hold(const char *what, int ms, pattern_fn redraw)
{
    int64_t t_end = esp_timer_get_time() + ms * 1000LL;
    while (esp_timer_get_time() < t_end) {
        if (redraw) redraw();
        ESP_LOGI(TAG, "  %-22s frames=%lu  %.0f Hz", what,
                 (unsigned long)hub75_frame_count(), hub75_refresh_hz());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static uint8_t s_pat_r, s_pat_g, s_pat_b;
static int s_pat_y0, s_pat_y1;

static void draw_half(void)
{
    hub75_clear();
    for (int y = s_pat_y0; y < s_pat_y1; y++) {
        for (int x = 0; x < HUB75_WIDTH; x++) {
            hub75_set_pixel(x, y, s_pat_r, s_pat_g, s_pat_b);
        }
    }
}

static void draw_border(void)
{
    hub75_clear();
    for (int x = 0; x < HUB75_WIDTH; x++) {
        hub75_set_pixel(x, 0, 255, 255, 255);
        hub75_set_pixel(x, HUB75_HEIGHT - 1, 255, 255, 255);
    }
    for (int y = 0; y < HUB75_HEIGHT; y++) {
        hub75_set_pixel(0, y, 255, 255, 255);
        hub75_set_pixel(HUB75_WIDTH - 1, y, 255, 255, 255);
    }
}

/* Two things could explain a blank test frame while the plasma is visible:
 * the frame does not survive being drawn only once, or the solid fill browns
 * out the panel supply (a solid half is far more current than plasma, which is
 * mostly dark). These steps separate them -- the border is only ~380 LEDs, so
 * it cannot be a power problem. */
static void draw_persistence_test(void)
{
    ESP_LOGI(TAG, "A: border, drawn ONCE -- expect a white border for 3 s");
    draw_border();
    hold("static border", 3000, NULL);

    ESP_LOGI(TAG, "B: border, REDRAWN every 100 ms -- expect the same border");
    hold("redrawn border", 3000, draw_border);

    ESP_LOGI(TAG, "C: cleared -- expect black for 2 s");
    hub75_clear();
    hold("cleared", 2000, NULL);

    /* Current test. If A and B were fine but this one is dark, dim or
     * flickering, the panel supply is the problem, not the driver. */
    s_pat_r = s_pat_g = s_pat_b = 255;
    s_pat_y0 = 0;
    s_pat_y1 = HUB75_HEIGHT;
    ESP_LOGI(TAG, "D: full white at 25%% brightness -- expect dim white for 2 s");
    hub75_set_brightness(64);
    hold("white dim", 2000, draw_half);

    ESP_LOGI(TAG, "E: full white at full brightness -- watch for sag/flicker");
    hub75_set_brightness(255);
    hold("white bright", 2000, draw_half);

    hub75_set_brightness(160);
    hub75_clear();
}

/* Lights one colour in one half of the panel at a time. Each half is fed by
 * its own set of HUB75 data lines, so a channel that stays dark names the wire
 * to check: the top half is R1/G1/B1, the bottom half R2/G2/B2. */
static void draw_channel_test(void)
{
    const struct { uint8_t r, g, b; const char *colour; } chans[] = {
        {255, 0, 0, "R"}, {0, 255, 0, "G"}, {0, 0, 255, "B"},
    };
    for (int half = 0; half < 2; half++) {
        s_pat_y0 = half ? HUB75_ROWS : 0;
        s_pat_y1 = half ? HUB75_HEIGHT : HUB75_ROWS;
        for (int c = 0; c < 3; c++) {
            s_pat_r = chans[c].r;
            s_pat_g = chans[c].g;
            s_pat_b = chans[c].b;
            char what[32];
            snprintf(what, sizeof(what), "%s half %s (%s%d)",
                     half ? "bottom" : "top", chans[c].colour,
                     chans[c].colour, half ? 2 : 1);
            ESP_LOGI(TAG, "expect %s", what);
            hold(what, 1500, draw_half);
        }
    }

    ESP_LOGI(TAG, "expect a 1px white border touching all four edges");
    hold("border", 3000, draw_border);
    hub75_clear();
}

void app_main(void)
{
    ESP_ERROR_CHECK(hub75_init());
    ESP_ERROR_CHECK(hub75_start());

    draw_persistence_test();
    draw_channel_test();

    int64_t t0 = esp_timer_get_time();
    while (1) {
        float t = (esp_timer_get_time() - t0) / 1e6f;
        draw_plasma(t);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
