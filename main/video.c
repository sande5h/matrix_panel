#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "hub75.h"
#include "video.h"

static const char *TAG = "video";

/* Written by tools/encode_video.py. Little endian, 16 bytes. */
#define VIDEO_MAGIC 0x35374248u        /* "HB75" */
#define VIDEO_HEADER_BYTES 16

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t frames;
} __attribute__((packed)) video_header_t;

bool video_play(bool loop)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "video");
    if (!part) {
        ESP_LOGW(TAG, "no 'video' partition -- check partitions.csv is selected");
        return false;
    }

    video_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        hdr.magic != VIDEO_MAGIC) {
        ESP_LOGW(TAG, "no clip flashed: %s",
                 "run tools/encode_video.py, then parttool.py write_partition");
        return false;
    }
    if (hdr.width != HUB75_WIDTH || hdr.height != HUB75_HEIGHT) {
        ESP_LOGE(TAG, "clip is %ux%u, panel is %dx%d -- re-encode it",
                 hdr.width, hdr.height, HUB75_WIDTH, HUB75_HEIGHT);
        return false;
    }

    const size_t frame_bytes = (size_t)hdr.width * hdr.height * sizeof(uint16_t);
    const uint32_t period_us = 1000000u / (hdr.fps ? hdr.fps : 15);

    /* One frame in RAM at a time. Flash reads at ~240 KB/s for 15 fps, which
     * the SPI flash handles comfortably. */
    uint16_t *frame = heap_caps_malloc(frame_bytes, MALLOC_CAP_8BIT);
    if (!frame) {
        ESP_LOGE(TAG, "no room for a %u byte frame", (unsigned)frame_bytes);
        return false;
    }

    ESP_LOGI(TAG, "playing %lu frames, %ux%u @ %u fps (%.1f s)",
             (unsigned long)hdr.frames, hdr.width, hdr.height, hdr.fps,
             hdr.frames / (float)hdr.fps);

    do {
        int64_t next = esp_timer_get_time();
        for (uint32_t i = 0; i < hdr.frames; i++) {
            size_t off = VIDEO_HEADER_BYTES + (size_t)i * frame_bytes;
            if (esp_partition_read(part, off, frame, frame_bytes) != ESP_OK) {
                ESP_LOGE(TAG, "read failed at frame %lu", (unsigned long)i);
                free(frame);
                return false;
            }
            hub75_blit_rgb565(frame);

            /* Pace on absolute time so a slow frame does not accumulate. */
            next += period_us;
            int64_t wait = next - esp_timer_get_time();
            if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
        }
    } while (loop);

    free(frame);
    return true;
}
