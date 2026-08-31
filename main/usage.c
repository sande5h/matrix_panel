#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include "usage.h"
#include "net.h"
#include "secrets.h"

static const char *TAG = "usage";

/* The server caches the OAuth call for 60 s and its upstream 429s easily, so
 * polling faster would buy nothing and risk the lockout. */
#define POLL_SECONDS   60
#define RETRY_SECONDS  10
#define BODY_MAX       1024

static usage_t s_usage;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void parse(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "unparseable response");
        return;
    }

    usage_t u = { .valid = true, .updated_us = esp_timer_get_time() };

    const cJSON *src = cJSON_GetObjectItem(root, "src");
    u.stale = !cJSON_IsString(src) || strcmp(src->valuestring, "oauth") != 0;

    const struct { const char *key; int *pct; char *resets; size_t n; } fields[] = {
        {"session", &u.session_pct, u.session_resets, sizeof(u.session_resets)},
        {"weekly",  &u.weekly_pct,  u.weekly_resets,  sizeof(u.weekly_resets)},
    };
    for (int i = 0; i < 2; i++) {
        const cJSON *blob = cJSON_GetObjectItem(root, fields[i].key);
        if (!cJSON_IsObject(blob)) continue;
        const cJSON *pct = cJSON_GetObjectItem(blob, "pct");
        const cJSON *in  = cJSON_GetObjectItem(blob, "resets_in");
        if (cJSON_IsNumber(pct)) *fields[i].pct = (int)pct->valuedouble;
        if (cJSON_IsString(in)) strlcpy(fields[i].resets, in->valuestring, fields[i].n);
    }

    portENTER_CRITICAL(&s_lock);
    s_usage = u;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "session %d%% (%s), weekly %d%% (%s)%s",
             u.session_pct, u.session_resets, u.weekly_pct, u.weekly_resets,
             u.stale ? " [stale]" : "");
    cJSON_Delete(root);
}

static bool poll_once(void)
{
    esp_http_client_config_t cfg = {
        .url = USAGE_URL,
        .timeout_ms = 5000,
        .method = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;

    bool ok = false;
    char body[BODY_MAX];

    if (esp_http_client_open(c, 0) == ESP_OK) {
        int64_t len = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        int got = esp_http_client_read_response(c, body, sizeof(body) - 1);
        if (status == 200 && got > 0) {
            body[got] = '\0';
            parse(body);
            ok = true;
        } else {
            ESP_LOGW(TAG, "HTTP %d, %d bytes (content-length %lld)", status, got, len);
        }
    } else {
        ESP_LOGW(TAG, "cannot reach %s -- is ccusage_server.py running?", USAGE_URL);
    }

    esp_http_client_cleanup(c);
    return ok;
}

static void poll_task(void *arg)
{
    while (1) {
        if (net_connected()) {
            bool ok = poll_once();
            if (!ok) {
                portENTER_CRITICAL(&s_lock);
                s_usage.stale = true;      /* keep the last reading, flag it */
                portEXIT_CRITICAL(&s_lock);
            }
            vTaskDelay(pdMS_TO_TICKS((ok ? POLL_SECONDS : RETRY_SECONDS) * 1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

void usage_start(void)
{
    xTaskCreate(poll_task, "usage", 4096, NULL, 4, NULL);
}

bool usage_get(usage_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_usage;
    portEXIT_CRITICAL(&s_lock);
    return out->valid;
}
