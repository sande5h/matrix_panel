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

/* tjpgd needs a scratch pool; ~3.1 KiB is the documented minimum for baseline
 * JPEG, and this is not worth economising on. */
#define JPEG_WORK_BYTES 4096

/* Set for as long as the player is reading the partition, so an upload can
 * wait it out. Written only by the render task, read only by the HTTP task. */
static volatile bool s_busy;

/* Handed to tjpgd as its "device": one compressed frame in, one RGB888 frame
 * out. The ROM decoder emits RGB888 and the panel's gamma table takes 8 bit
 * channels, so nothing converts -- an RGB565 round trip here used to cost
 * three bits of red and blue for nothing. */
typedef struct {
    const uint8_t *jpeg;
    size_t size;
    size_t pos;
    uint8_t *out;
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
    const size_t run = (size_t)(rect->right - rect->left + 1) * 3;

    /* A row of the MCU at a time. Same layout on both sides, so this is a
     * copy rather than the per-pixel conversion it used to be. */
    for (int y = rect->top; y <= rect->bottom; y++) {
        memcpy(c->out + ((size_t)y * HUB75_WIDTH + rect->left) * 3, src, run);
        src += run;
    }
    return 1;
}

static bool play_raw(const esp_partition_t *part, const video_header_t *hdr,
                     bool (*keep_going)(void))
{
    const size_t frame_bytes = (size_t)hdr->width * hdr->height * sizeof(uint16_t);
    const uint32_t period_us = 1000000u / hdr->fps;

    uint16_t *frame = heap_caps_malloc(frame_bytes, MALLOC_CAP_8BIT);
    if (!frame) return false;

    int64_t next = esp_timer_get_time();
    for (uint32_t i = 0; i < hdr->frames; i++) {
        if (keep_going && !keep_going()) break;
        size_t off = VIDEO_HEADER_BYTES + (size_t)i * frame_bytes;
        if (esp_partition_read(part, off, frame, frame_bytes) != ESP_OK) break;
        hub75_blit_rgb565(frame);

        next += period_us;                    /* absolute pacing: no drift */
        int64_t wait = next - esp_timer_get_time();
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait / 1000));
    }
    free(frame);
    return true;
}

static bool play_mjpeg(const esp_partition_t *part, const video_header_t *hdr,
                       bool (*keep_going)(void))
{
    const uint32_t period_us = 1000000u / hdr->fps;
    bool ok = false;

    /* The whole size index at once: 4 bytes a frame, so a few KiB, and it
     * saves a flash read per frame. */
    size_t index_bytes = (size_t)hdr->frames * sizeof(uint32_t);
    uint32_t *sizes = heap_caps_malloc(index_bytes, MALLOC_CAP_8BIT);
    uint8_t *frame = heap_caps_malloc((size_t)hdr->width * hdr->height * 3, MALLOC_CAP_8BIT);
    void *work = heap_caps_malloc(JPEG_WORK_BYTES, MALLOC_CAP_8BIT);
    uint8_t *jpeg = NULL;

    if (!sizes || !frame || !work) {
        ESP_LOGE(TAG, "out of memory for the decoder");
        goto done;
    }
    if (esp_partition_read(part, VIDEO_HEADER_BYTES, sizes, index_bytes) != ESP_OK) {
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

    size_t off = VIDEO_HEADER_BYTES + index_bytes;
    int64_t next = esp_timer_get_time();
    int64_t mark = next;
    int64_t busy = 0;          /* time actually spent decoding, per heartbeat */

    for (uint32_t i = 0; i < hdr->frames; i++) {
        if (keep_going && !keep_going()) { ok = true; goto done; }

        int64_t t0 = esp_timer_get_time();
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
            hub75_blit_rgb888(frame);
        }
        busy += esp_timer_get_time() - t0;

        /* Periodic heartbeat: the achieved rate, whether or not it is keeping
         * up. A late frame does not corrupt anything, it just slows playback,
         * so without this the difference is invisible.
         *
         * The second number is how much of each frame's budget the decode and
         * blit actually eat. That is the one that says whether the frame rate
         * can go up: at 100% there is nothing left and playback starts to
         * slip, whatever the achieved rate says this second. */
        if ((i % 128) == 127) {
            int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "frame %lu/%lu, %.1f fps achieved (%u target), "
                          "%.1f ms/frame, %.0f%% of budget",
                     (unsigned long)i, (unsigned long)hdr->frames,
                     128.0e6f / (float)(now - mark), hdr->fps,
                     busy / 128 / 1000.0f, 100.0f * busy / (float)(now - mark));
            mark = now;
            busy = 0;
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

static const esp_partition_t *video_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_ANY, "video");
}

bool video_idle(void)
{
    return !s_busy;
}

bool video_info(video_header_t *out)
{
    const esp_partition_t *part = video_partition();
    video_header_t hdr;

    if (!part || esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) return false;
    if (hdr.magic != VIDEO_MAGIC_RAW && hdr.magic != VIDEO_MAGIC_MJPEG) return false;

    if (out) *out = hdr;
    return true;
}

bool video_play(bool loop, bool (*keep_going)(void))
{
    const esp_partition_t *part = video_partition();
    if (!part) {
        ESP_LOGW(TAG, "no 'video' partition -- check partitions.csv is selected");
        return false;
    }

    video_header_t hdr;
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK ||
        (hdr.magic != VIDEO_MAGIC_RAW && hdr.magic != VIDEO_MAGIC_MJPEG)) {
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
             hdr.magic == VIDEO_MAGIC_MJPEG ? "mjpeg" : "raw",
             (unsigned long)hdr.frames, hdr.width, hdr.height, hdr.fps,
             hdr.frames / (float)hdr.fps);

    /* Held across the whole loop, not per frame: an upload must not erase the
     * partition between two frames either. */
    s_busy = true;
    bool ok;
    do {
        ok = (hdr.magic == VIDEO_MAGIC_MJPEG) ? play_mjpeg(part, &hdr, keep_going)
                                              : play_raw(part, &hdr, keep_going);
    } while (ok && loop && (!keep_going || keep_going()));
    s_busy = false;

    return ok;
}
