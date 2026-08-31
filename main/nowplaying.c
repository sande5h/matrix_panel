#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include "nowplaying.h"

static const char *TAG = "nowplaying";

static nowplaying_t s_np;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void copy_str(char *dst, size_t n, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, n);
    } else {
        dst[0] = '\0';
    }
}

/* Anything off the wire is clamped to a plausible range here, so the render
 * side never has to defend against a negative or absurd duration. */
static int get_secs(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(item)) return 0;
    double v = item->valuedouble;
    if (v < 0) return 0;
    if (v > 24 * 3600) return 24 * 3600;
    return (int)v;
}

bool nowplaying_update(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "unparseable body");
        return false;
    }

    nowplaying_t np = { .updated_us = esp_timer_get_time() };

    const cJSON *stopped = cJSON_GetObjectItem(root, "stopped");
    if (cJSON_IsTrue(stopped)) {
        /* An explicit stop clears immediately, rather than waiting out the
         * staleness window. */
        np.valid = false;
    } else {
        copy_str(np.title,  sizeof(np.title),  root, "title");
        copy_str(np.artist, sizeof(np.artist), root, "artist");
        np.position_s = get_secs(root, "position");
        np.duration_s = get_secs(root, "duration");
        np.playing = cJSON_IsTrue(cJSON_GetObjectItem(root, "playing"));
        np.valid = np.title[0] != '\0';
    }

    portENTER_CRITICAL(&s_lock);
    s_np = np;
    portEXIT_CRITICAL(&s_lock);

    if (np.valid) {
        ESP_LOGI(TAG, "%s -- %s (%d/%ds)%s", np.title, np.artist,
                 np.position_s, np.duration_s, np.playing ? "" : " [paused]");
    } else {
        ESP_LOGI(TAG, "stopped");
    }

    cJSON_Delete(root);
    return true;
}

bool nowplaying_get(nowplaying_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_np;
    portEXIT_CRITICAL(&s_lock);

    if (!out->valid) return false;

    int64_t age_us = esp_timer_get_time() - out->updated_us;
    if (age_us > (int64_t)NOWPLAYING_STALE_S * 1000000) {
        out->valid = false;      /* the Mac went away; do not show a ghost */
        return false;
    }
    return true;
}
