#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "screen.h"

static const char *TAG = "screen";
static const char *const NAMES[SCREEN_COUNT] = { "clock", "video" };

/* A plain volatile int is enough: single word, written by the HTTP task and
 * read by the render loop, with no invariant spanning two fields. */
static volatile screen_t s_current = SCREEN_CLOCK;

screen_t screen_get(void)
{
    return s_current;
}

void screen_set(screen_t s)
{
    if (s < 0 || s >= SCREEN_COUNT || s == s_current) return;
    ESP_LOGI(TAG, "%s -> %s", NAMES[s_current], NAMES[s]);
    s_current = s;
}

screen_t screen_next(void)
{
    screen_set((screen_t)((s_current + 1) % SCREEN_COUNT));
    return s_current;
}

const char *screen_name(screen_t s)
{
    return (s >= 0 && s < SCREEN_COUNT) ? NAMES[s] : "?";
}

bool screen_from_name(const char *name, screen_t *out)
{
    for (int i = 0; i < SCREEN_COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) {
            *out = (screen_t)i;
            return true;
        }
    }
    return false;
}
