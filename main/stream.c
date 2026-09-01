#include <errno.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "rom/tjpgd.h"

#include "hub75.h"
#include "screen.h"
#include "stream.h"

static const char *TAG = "stream";

#define PORT        8089
#define MAGIC_RAW   "HB7S"      /* RGB888, exactly FRAME_BYTES of it        */
#define MAGIC_JPEG  "HB7Z"      /* u32 length, then that many bytes of JPEG */
#define MAGIC_LEN   4
#define FRAME_BYTES (HUB75_WIDTH * HUB75_HEIGHT * 3)

/* Raw is the best this panel can look -- no compression error at all -- but it
 * costs 5.9 Mbps at 30 fps, and the refresh DMA leaves nowhere near that. A
 * JPEG frame is about an eighth the size, which fits, and the ROM decoder has
 * already shown it can keep up at 60 fps from flash. So the sender picks:
 * quality when the link allows it, frame rate when it does not. */
#define JPEG_MAX    16384
#define JPEG_WORK   4096

/* A sender that stops mid-frame without closing -- wifi dropping out, the Mac
 * sleeping -- would otherwise leave the panel parked on the stream screen
 * forever. Five seconds is far longer than any real gap between frames. */
#define RECV_TIMEOUT_S 5

static volatile bool s_active;

/* Where a frame's time actually goes. Guessing at this cost three rounds of
 * plausible-sounding config changes that each did nothing, so measure it:
 * a long wait means the bytes are not arriving, while a short wait spread over
 * many calls means the cost is per-call and the bytes are already here. */
static int64_t  s_wait_us;
static uint32_t s_recvs;

bool stream_active(void)
{
    return s_active;
}

/* Same decoder plumbing as the flashed-clip player: one compressed frame in,
 * one RGB888 frame out, no colour conversion because tjpgd already emits it. */
typedef struct {
    const uint8_t *jpeg;
    size_t size, pos;
    uint8_t *out;
} jctx_t;

static UINT jpeg_in(JDEC *jd, BYTE *buf, UINT len)
{
    jctx_t *c = (jctx_t *)jd->device;
    size_t left = c->size - c->pos;
    if (len > left) len = left;
    if (buf) memcpy(buf, c->jpeg + c->pos, len);
    c->pos += len;
    return len;
}

static UINT jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jctx_t *c = (jctx_t *)jd->device;
    const uint8_t *src = bitmap;
    const size_t run = (size_t)(rect->right - rect->left + 1) * 3;

    for (int y = rect->top; y <= rect->bottom; y++) {
        memcpy(c->out + ((size_t)y * HUB75_WIDTH + rect->left) * 3, src, run);
        src += run;
    }
    return 1;
}

/* recv returns what it has, not what was asked for, so every read loops. */
static bool read_all(int sock, uint8_t *dst, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int64_t t0 = esp_timer_get_time();
        int r = recv(sock, dst + got, n - got, 0);
        s_wait_us += esp_timer_get_time() - t0;
        s_recvs++;
        if (r <= 0) return false;       /* closed, or the timeout expired */
        got += (size_t)r;
    }
    return true;
}

/* Every frame carries a magic word. TCP does not lose bytes, so this is not
 * about corruption -- it catches a sender whose frames are the wrong size,
 * which would otherwise skew the picture a little further on every frame and
 * look like a hardware fault. Resyncing costs one byte at a time and is
 * bounded, so a garbage stream disconnects instead of spinning. */
static bool sync_frame(int sock, bool *is_jpeg)
{
    uint8_t w[MAGIC_LEN];
    if (!read_all(sock, w, MAGIC_LEN)) return false;

    int slid = 0;
    while (memcmp(w, MAGIC_RAW, MAGIC_LEN) != 0 &&
           memcmp(w, MAGIC_JPEG, MAGIC_LEN) != 0) {
        w[0] = w[1]; w[1] = w[2]; w[2] = w[3];
        if (!read_all(sock, w + MAGIC_LEN - 1, 1)) return false;
        if (++slid == 1) {
            ESP_LOGW(TAG, "out of step -- is the sender scaling to %dx%d?",
                     HUB75_WIDTH, HUB75_HEIGHT);
        }
        if (slid > FRAME_BYTES) {
            ESP_LOGE(TAG, "no frame header in %d bytes, dropping the sender",
                     slid);
            return false;
        }
    }
    *is_jpeg = (memcmp(w, MAGIC_JPEG, MAGIC_LEN) == 0);
    return true;
}

static void serve(int sock, uint8_t *frame, uint8_t *jpeg, void *work)
{
    /* Where to go back to when the sender leaves. A stream arriving during an
     * upload should not strand the panel on the update screen. */
    screen_t prev = screen_get();
    if (prev >= SCREEN_CYCLE_COUNT) prev = SCREEN_CLOCK;

    s_active = true;
    screen_set(SCREEN_STREAM);
    ESP_LOGI(TAG, "sender connected");

    uint32_t frames = 0;
    int64_t mark = esp_timer_get_time();
    int64_t busy = 0;
    s_wait_us = 0;
    s_recvs = 0;

    /* Leaves as soon as the screen changes under it, so toggling away from a
     * stream drops the connection rather than reading frames nobody sees. */
    while (screen_get() == SCREEN_STREAM) {
        bool is_jpeg = false;
        if (!sync_frame(sock, &is_jpeg)) break;

        uint32_t len = 0;
        if (is_jpeg) {
            uint8_t n[4];
            if (!read_all(sock, n, 4)) break;
            len = (uint32_t)n[0] | ((uint32_t)n[1] << 8) |
                  ((uint32_t)n[2] << 16) | ((uint32_t)n[3] << 24);
            if (len == 0 || len > JPEG_MAX) {
                ESP_LOGE(TAG, "frame claims %lu bytes, dropping the sender",
                         (unsigned long)len);
                break;
            }
            if (!read_all(sock, jpeg, len)) break;
        } else {
            if (!read_all(sock, frame, FRAME_BYTES)) break;
        }

        int64_t t0 = esp_timer_get_time();
        if (is_jpeg) {
            jctx_t ctx = { .jpeg = jpeg, .size = len, .pos = 0, .out = frame };
            JDEC jd;
            JRESULT res = jd_prepare(&jd, jpeg_in, work, JPEG_WORK, &ctx);
            if (res == JDR_OK) res = jd_decomp(&jd, jpeg_out, 0);
            if (res != JDR_OK) {
                ESP_LOGW(TAG, "frame did not decode (tjpgd %d)", res);
                continue;           /* hold the last frame, keep the sender */
            }
        }
        hub75_blit_rgb888(frame);
        busy += esp_timer_get_time() - t0;

        /* Periodic heartbeat: the rate frames are arriving at, and what the
         * blit costs. The gap between them is the network, so the two numbers
         * together say which side is the limit. */
        if ((++frames % 64) == 0) {
            int64_t now = esp_timer_get_time();
            /* "decode+blit" because a JPEG frame is decoded inside the same
             * measurement -- calling it blit made a 12 ms number look like the
             * blit had got twice as slow, when the blit had not changed. */
            ESP_LOGI(TAG, "%lu frames, %.1f fps received | decode+blit %.2f ms | "
                          "socket %.1f ms over %.1f recvs (%.0f B each)",
                     (unsigned long)frames, 64.0e6f / (float)(now - mark),
                     busy / 64 / 1000.0f, s_wait_us / 64 / 1000.0f,
                     s_recvs / 64.0f,
                     s_recvs ? (float)(FRAME_BYTES * 64) / s_recvs : 0.0f);
            mark = now;
            busy = 0;
            s_wait_us = 0;
            s_recvs = 0;
        }
    }

    ESP_LOGI(TAG, "sender gone after %lu frames", (unsigned long)frames);
    s_active = false;
    if (screen_get() == SCREEN_STREAM) screen_set(prev);
}

static void listener(void *arg)
{
    (void)arg;

    /* One frame, allocated once. Internal RAM: it is written by the socket and
     * read by the blit on every frame. */
    uint8_t *frame = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_8BIT);
    uint8_t *jpeg  = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_8BIT);
    void    *work  = heap_caps_malloc(JPEG_WORK, MALLOC_CAP_8BIT);
    if (!frame || !jpeg || !work) {
        ESP_LOGE(TAG, "no room for the frame buffers");
        free(frame); free(jpeg); free(work);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls < 0) {
            ESP_LOGE(TAG, "socket: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
            listen(ls, 1) < 0) {
            ESP_LOGE(TAG, "bind/listen on %d: errno %d", PORT, errno);
            close(ls);
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "listening on %d", PORT);

        /* One sender at a time. A second connection waits in the backlog and
         * is served when the first leaves, which is friendlier than refusing
         * it and gives no way to interrupt a stream by accident. */
        while (1) {
            struct sockaddr_storage from;
            socklen_t flen = sizeof(from);
            int cs = accept(ls, (struct sockaddr *)&from, &flen);
            if (cs < 0) {
                ESP_LOGW(TAG, "accept: errno %d", errno);
                break;                      /* rebuild the listening socket */
            }

            struct timeval tv = { .tv_sec = RECV_TIMEOUT_S };
            setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            serve(cs, frame, jpeg, work);
            close(cs);
        }
        close(ls);
    }
}

void stream_start(void)
{
    xTaskCreate(listener, "stream", 4096, NULL, 5, NULL);
}
