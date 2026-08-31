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
                    + sinf(sqrtf((float)((x - 32) * (x - 32) + (y - 32) * (y - 32))) * 0.22f - t);
            uint8_t hue = (uint8_t)((v + 4.0f) * (255.0f / 8.0f));
            uint8_t r, g, b;
            hue_rgb(hue, &r, &g, &b);
            hub75_set_pixel(x, y, r, g, b);
        }
    }
}

/* Solid colour walk-through -- the first thing to look at on a new panel. */
static void draw_smoke_test(void)
{
    const struct { uint8_t r, g, b; const char *name; } steps[] = {
        {255, 0, 0, "red"}, {0, 255, 0, "green"}, {0, 0, 255, "blue"},
        {255, 255, 255, "white"},
    };
    for (int i = 0; i < 4; i++) {
        ESP_LOGI(TAG, "smoke test: %s", steps[i].name);
        hub75_fill(steps[i].r, steps[i].g, steps[i].b);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    hub75_clear();
}

void app_main(void)
{
    ESP_ERROR_CHECK(hub75_init());
    ESP_ERROR_CHECK(hub75_start());

    draw_smoke_test();

    int64_t t0 = esp_timer_get_time();
    while (1) {
        float t = (esp_timer_get_time() - t0) / 1e6f;
        draw_plasma(t);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
