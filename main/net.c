#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "net.h"
#include "secrets.h"

static const char *TAG = "net";

/* Nepal is UTC+5:45 and has no daylight saving. POSIX inverts the sign of the
 * offset in a TZ string, so +5:45 is written -5:45. */
#define TZ_STRING "NPT-5:45"

#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_events;
static char s_ip[16] = "0.0.0.0";
static bool s_time_valid;
static int s_retries;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        strcpy(s_ip, "0.0.0.0");
        /* Retry forever, but log sparsely: a panel on a shelf should heal
         * from a router reboot without anyone touching it. */
        if (++s_retries % 10 == 1) {
            ESP_LOGW(TAG, "disconnected, retrying (attempt %d)", s_retries);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        ESP_LOGI(TAG, "connected to %s, ip %s", WIFI_SSID, s_ip);
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t net_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_wifi, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to %s", WIFI_SSID);
    return ESP_OK;
}

bool net_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_CONNECTED);
}

bool net_wait_connected(int timeout_ms)
{
    if (!s_events) return false;
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(timeout_ms)) & BIT_CONNECTED;
}

const char *net_ip(void)
{
    return s_ip;
}

esp_err_t net_sync_time(int timeout_ms)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) != ESP_OK) {
        ESP_LOGW(TAG, "no SNTP reply within %d ms", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    setenv("TZ", TZ_STRING, 1);
    tzset();
    s_time_valid = true;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d local",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return ESP_OK;
}

bool net_time_valid(void)
{
    return s_time_valid;
}
