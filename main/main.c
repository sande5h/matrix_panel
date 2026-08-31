#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hub75.h"

static const char *TAG = "main";

/* 6 bit hue -> RGB, no saturation/value control, just enough for a demo. */
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

/* A plasma field -- exercises every colour bit on every pixel. */
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

/* Quick confidence check at boot: each half in each primary, then a border
 * whose edges must land on the outermost rows and columns. */
static void self_test(void)
{
    const struct { uint8_t r, g, b; const char *name; } chans[] = {
        {255, 0, 0, "red"}, {0, 255, 0, "green"}, {0, 0, 255, "blue"},
    };
    for (int half = 0; half < 2; half++) {
        for (int c = 0; c < 3; c++) {
            ESP_LOGI(TAG, "%s half %s", half ? "bottom" : "top", chans[c].name);
            hub75_clear();
            for (int y = half ? HUB75_ROWS : 0; y < (half ? HUB75_HEIGHT : HUB75_ROWS); y++) {
                for (int x = 0; x < HUB75_WIDTH; x++) {
                    hub75_set_pixel(x, y, chans[c].r, chans[c].g, chans[c].b);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(600));
        }
    }

    ESP_LOGI(TAG, "border");
    hub75_clear();
    for (int x = 0; x < HUB75_WIDTH; x++) {
        hub75_set_pixel(x, 0, 255, 255, 255);
        hub75_set_pixel(x, HUB75_HEIGHT - 1, 255, 255, 255);
    }
    for (int y = 0; y < HUB75_HEIGHT; y++) {
        hub75_set_pixel(0, y, 255, 255, 255);
        hub75_set_pixel(HUB75_WIDTH - 1, y, 255, 255, 255);
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    hub75_clear();
}

void app_main(void)
{
    ESP_ERROR_CHECK(hub75_init());
    ESP_ERROR_CHECK(hub75_start());
    hub75_set_brightness(128);

    self_test();

    int64_t t0 = esp_timer_get_time();
    int64_t next_log = 0;
    while (1) {
        int64_t now = esp_timer_get_time();
        draw_plasma((now - t0) / 1e6f);
        if (now >= next_log) {          /* periodic heartbeat, not change gated */
            ESP_LOGI(TAG, "refresh %.0f Hz, %lu frames",
                     hub75_refresh_hz(), (unsigned long)hub75_frame_count());
            next_log = now + 5000000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
