#include <math.h>

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

/* Lights one colour in one half of the panel at a time. Each half is fed by
 * its own set of HUB75 data lines, so a channel that stays dark names the wire
 * to check: the top half is R1/G1/B1, the bottom half R2/G2/B2. */
static void draw_channel_test(void)
{
    const struct { uint8_t r, g, b; const char *colour; } chans[] = {
        {255, 0, 0, "R"}, {0, 255, 0, "G"}, {0, 0, 255, "B"},
    };
    for (int half = 0; half < 2; half++) {
        int y0 = half ? HUB75_ROWS : 0;
        int y1 = half ? HUB75_HEIGHT : HUB75_ROWS;
        for (int c = 0; c < 3; c++) {
            ESP_LOGI(TAG, "expect %s half all %s  (signal %s%d, GPIO check)",
                     half ? "bottom" : "top", chans[c].colour,
                     chans[c].colour, half ? 2 : 1);
            hub75_clear();
            for (int y = y0; y < y1; y++) {
                for (int x = 0; x < HUB75_WIDTH; x++) {
                    hub75_set_pixel(x, y, chans[c].r, chans[c].g, chans[c].b);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
    }

    /* A one pixel border plus corner marks: any horizontal shift shows up as
     * the left or right edge landing in the wrong column. */
    ESP_LOGI(TAG, "expect a 1px white border touching all four edges");
    hub75_clear();
    for (int x = 0; x < HUB75_WIDTH; x++) {
        hub75_set_pixel(x, 0, 255, 255, 255);
        hub75_set_pixel(x, HUB75_HEIGHT - 1, 255, 255, 255);
    }
    for (int y = 0; y < HUB75_HEIGHT; y++) {
        hub75_set_pixel(0, y, 255, 255, 255);
        hub75_set_pixel(HUB75_WIDTH - 1, y, 255, 255, 255);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    hub75_clear();
}

void app_main(void)
{
    ESP_ERROR_CHECK(hub75_init());
    ESP_ERROR_CHECK(hub75_start());

    draw_channel_test();

    int64_t t0 = esp_timer_get_time();
    while (1) {
        float t = (esp_timer_get_time() - t0) / 1e6f;
        draw_plasma(t);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
