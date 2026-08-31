#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rom/tjpgd.h"

#include "hub75.h"
#include "video.h"

static const char *TAG = "video";

/* Written by tools/encode_video.py. Little endian, 16 byte header. */
#define MAGIC_RAW   0x35374248u        /* "HB75" -- RGB565 frames, no index   */
#define MAGIC_MJPEG 0x4A374248u        /* "HB7J" -- JPEG frames, u32 size each */
#define HEADER_BYTES 16

/* tjpgd needs a scratch pool; ~3.1 KiB is the documented minimum for baseline
 * JPEG, and this is not worth economising on. */
#define JPEG_WORK_BYTES 4096

typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t frames;
} __attribute__((packed)) video_header_t;

/* Handed to tjpgd as its "device": one compressed frame in, one RGB565 frame
 * out. The ROM decoder emits RGB888, so the output callback converts. */
typedef struct {
    const uint8_t *jpeg;
    size_t size;
    size_t pos;
    uint16_t *out;
} jpeg_ctx_t;

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    size_t left = c->size - c->pos;
    if (len > left) len = left;
    if (buf) memcpy(buf, c->jpeg + c->pos, len);   /* NULL means skip ahead */
    c->pos += len;
    return len;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *c = (jpeg_ctx_t *)jd->device;
    const uint8_t *src = bitmap;

    for (int y = rect->top; y <= rect->bottom; y++) {
        uint16_t *dst = c->out + (size_t)y * HUB75_WIDTH + rect->left;
        for (int x = rect->left; x <= rect->right; x++) {
            uint8_t r = *src++, g = *src++, b = *src++;
            *dst++ = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
    return 1;
}

static bool play_raw(const esp_partition_t *part, const video_header_t *hdr)
{
    const size_t frame_bytes = (size_t)hdr->width * hdr->height * sizeof(uint16_t);
    const uint32_t period_us = 1000000u / hdr->fps;

    uint16_t *frame = heap_caps_malloc(frame_bytes, MALLOC_CAP_8BIT);
    if (!frame) return false;

    int64_t next = esp_timer_get_time();
    for (uint32_t i = 0; i < hdr->frames; i++) {
        size_t off = HEADER_BYTES + (size_t)i * frame_bytes;
        if (esp_partition_read(part, off, frame, frame_bytes) != ESP_OK) break;
        hub75_blit_rgb565(frame);

        next += period_us;                    /* absolute pacing: no drift */
        int64_t wait = next - esp_timer_get_time();
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
    }
    free(frame);
    return true;
}

static bool play_mjpeg(const esp_partition_t *part, const video_header_t *hdr)
{
    const uint32_t period_us = 1000000u / hdr->fps;
    bool ok = false;

    /* The whole size index at once: 4 bytes a frame, so a few KiB, and it
     * saves a flash read per frame. */
    size_t index_bytes = (size_t)hdr->frames * sizeof(uint32_t);
    uint32_t *sizes = heap_caps_malloc(index_bytes, MALLOC_CAP_8BIT);
    uint16_t *frame = heap_caps_malloc((size_t)hdr->width * hdr->height * 2, MALLOC_CAP_8BIT);
    void *work = heap_caps_malloc(JPEG_WORK_BYTES, MALLOC_CAP_8BIT);
    uint8_t *jpeg = NULL;

    if (!sizes || !frame || !work) {
        ESP_LOGE(TAG, "out of memory for the decoder");
        goto done;
    }
    if (esp_partition_read(part, HEADER_BYTES, sizes, index_bytes) != ESP_OK) {
        ESP_LOGE(TAG, "could not read the frame index");
        goto done;
    }

    uint32_t biggest = 0;
    for (uint32_t i = 0; i < hdr->frames; i++) {
        if (sizes[i] > biggest) biggest = sizes[i];
    }
    jpeg = heap_caps_malloc(biggest, MALLOC_CAP_8BIT);
    if (!jpeg) {
        ESP_LOGE(TAG, "no room for a %lu byte frame", (unsigned long)biggest);
        goto done;
    }
    ESP_LOGI(TAG, "mjpeg: largest frame %lu bytes", (unsigned long)biggest);

    size_t off = HEADER_BYTES + index_bytes;
    int64_t next = esp_timer_get_time();

    for (uint32_t i = 0; i < hdr->frames; i++) {
        if (esp_partition_read(part, off, jpeg, sizes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "read failed at frame %lu", (unsigned long)i);
            goto done;
        }
        off += sizes[i];

        jpeg_ctx_t ctx = { .jpeg = jpeg, .size = sizes[i], .pos = 0, .out = frame };
        JDEC jd;
        JRESULT res = jd_prepare(&jd, jpeg_in, work, JPEG_WORK_BYTES, &ctx);
        if (res == JDR_OK) res = jd_decomp(&jd, jpeg_out, 0);
        if (res != JDR_OK) {
            ESP_LOGW(TAG, "frame %lu did not decode (tjpgd %d)", (unsigned long)i, res);
        } else {
            hub75_blit_rgb565(frame);
        }

        next += period_us;
        int64_t wait = next - esp_timer_get_time();
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
    }
    ok = true;

done:
    free(sizes);
    free(frame);
    free(work);
    free(jpeg);
    return ok;
}

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
        (hdr.magic != MAGIC_RAW && hdr.magic != MAGIC_MJPEG)) {
        ESP_LOGW(TAG, "no clip flashed: run tools/encode_video.py, then "
                      "parttool.py write_partition");
        return false;
    }
    if (hdr.width != HUB75_WIDTH || hdr.height != HUB75_HEIGHT || !hdr.fps) {
        ESP_LOGE(TAG, "clip is %ux%u @ %u fps, panel is %dx%d -- re-encode it",
                 hdr.width, hdr.height, hdr.fps, HUB75_WIDTH, HUB75_HEIGHT);
        return false;
    }

    ESP_LOGI(TAG, "%s: %lu frames, %ux%u @ %u fps (%.1f s)",
             hdr.magic == MAGIC_MJPEG ? "mjpeg" : "raw",
             (unsigned long)hdr.frames, hdr.width, hdr.height, hdr.fps,
             hdr.frames / (float)hdr.fps);

    do {
        bool ok = (hdr.magic == MAGIC_MJPEG) ? play_mjpeg(part, &hdr)
                                             : play_raw(part, &hdr);
        if (!ok) return false;
    } while (loop);

    return true;
}
