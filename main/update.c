#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "hub75.h"
#include "screen.h"
#include "update.h"
#include "video.h"

static const char *TAG = "update";

/* One flash page. Bigger buffers do not speed the transfer up -- wifi, not
 * flash, is the limit -- and this one is static because the HTTP task's stack
 * is nowhere near big enough to hold it. */
#define CHUNK 4096

static uint8_t s_buf[CHUNK];

/* Progress, published for the panel to draw. Only ever touched by the HTTP
 * task (one upload at a time, see s_active) and read by the render loop. */
static volatile bool s_active;
static volatile int  s_pct;
static const char *volatile s_what = "";
static const char *volatile s_msg;        /* static strings only */
static screen_t s_prev;
static esp_timer_handle_t s_clear_timer;

/* ---- progress plumbing -------------------------------------------------- */

static void clear_cb(void *arg)
{
    (void)arg;
    s_active = false;
    s_msg = NULL;
    screen_restore(s_prev);
}

static void progress_begin(const char *what)
{
    if (s_clear_timer) esp_timer_stop(s_clear_timer);
    s_prev = screen_get();
    if (s_prev == SCREEN_UPDATE) s_prev = SCREEN_CLOCK;   /* a retry after a failure */
    s_what = what;
    s_pct = 0;
    s_msg = NULL;
    s_active = true;
    screen_set(SCREEN_UPDATE);
}

/* err == NULL means it worked. A failure stays on the panel for a few seconds
 * so a push from a script is not silently invisible. */
static void progress_end(const char *err, screen_t back)
{
    if (!err) {
        s_active = false;
        s_msg = NULL;
        screen_restore(back);
        return;
    }
    ESP_LOGE(TAG, "%s failed: %s", s_what, err);
    s_msg = err;
    s_prev = back;
    if (!s_clear_timer) {
        const esp_timer_create_args_t a = { .callback = clear_cb, .name = "upd_clear" };
        esp_timer_create(&a, &s_clear_timer);
    }
    if (s_clear_timer) esp_timer_start_once(s_clear_timer, 5 * 1000 * 1000);
}

bool update_progress(const char **what, int *pct, const char **msg)
{
    if (!s_active) return false;
    if (what) *what = s_what;
    if (pct)  *pct  = s_pct;
    if (msg)  *msg  = s_msg;
    return true;
}

void update_confirm(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (!run || esp_ota_get_state_partition(run, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "marked %s valid (%s)", run->label, esp_err_to_name(err));
}

/* ---- shared body handling ----------------------------------------------- */

/* Fills up to `want` bytes. Returns what it got, or -1 if the client vanished.
 * A timeout is not fatal: the panel is refreshing at 500 Hz and the sender can
 * legitimately stall for a moment. */
static int recv_some(httpd_req_t *req, void *dst, int want)
{
    for (int tries = 0; tries < 8; tries++) {
        int n = httpd_req_recv(req, dst, want);
        if (n > 0) return n;
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return -1;
    }
    return -1;
}

static esp_err_t reject(httpd_req_t *req, httpd_err_code_t code, const char *why)
{
    ESP_LOGW(TAG, "rejected: %s", why);
    httpd_resp_send_err(req, code, why);
    return ESP_FAIL;
}

/* ---- POST /ota ----------------------------------------------------------
 * The body is the raw matrix_panel.bin. It lands in whichever slot is not
 * running, and the panel reboots into it only once the whole image is in and
 * its checksum verifies -- a dropped connection leaves the running slot
 * untouched and bootable. */
static void reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "rebooting into the new image");
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot) {
        return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                      "no spare app slot: reflash the partition table over USB");
    }

    int total = req->content_len;
    if (total <= 0) return reject(req, HTTPD_400_BAD_REQUEST, "empty body");
    if ((size_t)total > slot->size) {
        return reject(req, HTTPD_413_CONTENT_TOO_LARGE, "image is bigger than the slot");
    }
    if (s_active) return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "an update is already running");

    ESP_LOGI(TAG, "ota: %d bytes into %s", total, slot->label);
    progress_begin("OTA");

    esp_ota_handle_t h = 0;
    /* Sequential writes erase as they go, so a 3 MB slot does not stall the
     * request for seconds up front, and the image header is written last. */
    esp_err_t err = esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        progress_end(esp_err_to_name(err), s_prev);
        return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
    }

    int got = 0;
    while (got < total) {
        int want = total - got < CHUNK ? total - got : CHUNK;
        int n = recv_some(req, s_buf, want);
        if (n < 0) {
            esp_ota_abort(h);
            progress_end("transfer cut short", s_prev);
            return ESP_FAIL;
        }
        /* An app image starts with 0xE9. Catching it here means pushing a
         * video to /ota by mistake costs nothing. */
        if (got == 0 && s_buf[0] != 0xE9) {
            esp_ota_abort(h);
            progress_end("not an app image", s_prev);
            return reject(req, HTTPD_400_BAD_REQUEST, "not an esp32 app image");
        }
        err = esp_ota_write(h, s_buf, n);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            progress_end(esp_err_to_name(err), s_prev);
            return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
        }
        got += n;
        s_pct = (int)((int64_t)got * 100 / total);
    }

    err = esp_ota_end(h);          /* verifies the image before it is bootable */
    if (err != ESP_OK) {
        progress_end(err == ESP_ERR_OTA_VALIDATE_FAILED ? "image did not verify"
                                                        : esp_err_to_name(err), s_prev);
        return reject(req, HTTPD_400_BAD_REQUEST, "image did not verify");
    }
    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        progress_end(esp_err_to_name(err), s_prev);
        return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "could not set boot slot");
    }

    s_pct = 100;
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"slot\":\"%s\",\"bytes\":%d,\"rebooting\":true}",
                     slot->label, got);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);

    /* Deferred so the reply actually leaves the socket. */
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 700 * 1000);
    return ESP_OK;
}

/* ---- POST /video --------------------------------------------------------
 * The body is a .bin from tools/encode_video.py. The 16 byte header is held
 * back and written last, so an interrupted upload leaves an unplayable
 * partition rather than one that plays garbage.
 */
static esp_err_t video_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "video");
    if (!part) return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no video partition");

    int total = req->content_len;
    if (total <= VIDEO_HEADER_BYTES) return reject(req, HTTPD_400_BAD_REQUEST, "body too short");
    if ((size_t)total > part->size) {
        return reject(req, HTTPD_413_CONTENT_TOO_LARGE, "clip is bigger than the partition");
    }
    if (s_active) return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "an update is already running");

    bool play_after = false;
    char query[48], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "play", val, sizeof(val)) == ESP_OK) {
        play_after = (val[0] == '1' || val[0] == 't' || val[0] == 'y');
    }

    progress_begin("VIDEO");

    /* The header first, and validated before anything is erased: a mistyped
     * file is then a 400 with the old clip still playable. */
    video_header_t hdr;
    int have = 0;
    while (have < VIDEO_HEADER_BYTES) {
        int n = recv_some(req, (char *)&hdr + have, VIDEO_HEADER_BYTES - have);
        if (n < 0) { progress_end("transfer cut short", s_prev); return ESP_FAIL; }
        have += n;
    }
    if (hdr.magic != VIDEO_MAGIC_RAW && hdr.magic != VIDEO_MAGIC_MJPEG) {
        progress_end("not a clip", s_prev);
        return reject(req, HTTPD_400_BAD_REQUEST, "not an encode_video.py file");
    }
    if (hdr.width != HUB75_WIDTH || hdr.height != HUB75_HEIGHT || !hdr.fps || !hdr.frames) {
        progress_end("wrong size", s_prev);
        return reject(req, HTTPD_400_BAD_REQUEST, "clip does not match the panel");
    }

    /* The player holds the partition for a whole loop; wait it out rather than
     * erasing under it. progress_begin already moved the screen off video, so
     * this returns within a frame. */
    for (int i = 0; i < 60 && !video_idle(); i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!video_idle()) {
        progress_end("player would not stop", s_prev);
        return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "player busy");
    }

    size_t erase = ((size_t)total + 4095) & ~(size_t)4095;
    esp_err_t err = esp_partition_erase_range(part, 0, erase);
    if (err != ESP_OK) {
        progress_end(esp_err_to_name(err), s_prev);
        return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "erase failed");
    }
    ESP_LOGI(TAG, "video: %d bytes, %lu frames @ %u fps, erased %u KiB",
             total, (unsigned long)hdr.frames, hdr.fps, (unsigned)(erase / 1024));

    /* Offsets stay 4 byte aligned: the body starts at 16 and every flush but
     * the last is a full page. */
    size_t off = VIDEO_HEADER_BYTES;
    int got = VIDEO_HEADER_BYTES, fill = 0;
    while (got < total) {
        int want = total - got < CHUNK - fill ? total - got : CHUNK - fill;
        int n = recv_some(req, s_buf + fill, want);
        if (n < 0) { progress_end("transfer cut short", s_prev); return ESP_FAIL; }
        fill += n;
        got += n;

        if (fill == CHUNK) {
            err = esp_partition_write(part, off, s_buf, CHUNK);
            if (err != ESP_OK) {
                progress_end(esp_err_to_name(err), s_prev);
                return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
            }
            off += CHUNK;
            fill = 0;
        }
        s_pct = (int)((int64_t)got * 100 / total);
    }
    if (fill) {
        int pad = (4 - (fill & 3)) & 3;         /* erased flash, so 0xFF is free */
        memset(s_buf + fill, 0xFF, pad);
        err = esp_partition_write(part, off, s_buf, fill + pad);
        if (err != ESP_OK) {
            progress_end(esp_err_to_name(err), s_prev);
            return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed");
        }
    }

    /* Last, so the clip only becomes playable once all of it is there. */
    err = esp_partition_write(part, 0, &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        progress_end(esp_err_to_name(err), s_prev);
        return reject(req, HTTPD_500_INTERNAL_SERVER_ERROR, "header write failed");
    }

    s_pct = 100;
    progress_end(NULL, play_after ? SCREEN_VIDEO : s_prev);

    char body[160];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"bytes\":%d,\"frames\":%lu,\"fps\":%u,"
                     "\"kind\":\"%s\",\"playing\":%s}",
                     got, (unsigned long)hdr.frames, hdr.fps,
                     hdr.magic == VIDEO_MAGIC_MJPEG ? "mjpeg" : "raw",
                     play_after ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, n);
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t a = { .callback = reboot_cb, .name = "reboot" };
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, 500 * 1000);
    return ESP_OK;
}

/* ---- status ------------------------------------------------------------- */

int update_json(char *buf, size_t len)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();

    esp_ota_img_states_t state = ESP_OTA_IMG_VALID;
    if (run) esp_ota_get_state_partition(run, &state);

    video_header_t hdr;
    bool clip = video_info(&hdr);

    return snprintf(buf, len,
        "{\"slot\":\"%s\",\"version\":\"%s\",\"built\":\"%s %s\",\"idf\":\"%s\","
        "\"pending_verify\":%s,"
        "\"clip\":{\"valid\":%s,\"kind\":\"%s\",\"fps\":%u,\"frames\":%lu}}",
        run ? run->label : "?", app->version, app->date, app->time, app->idf_ver,
        state == ESP_OTA_IMG_PENDING_VERIFY ? "true" : "false",
        clip ? "true" : "false",
        clip ? (hdr.magic == VIDEO_MAGIC_MJPEG ? "mjpeg" : "raw") : "none",
        clip ? hdr.fps : 0, clip ? (unsigned long)hdr.frames : 0UL);
}

void update_register(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        { .uri = "/ota",    .method = HTTP_POST, .handler = ota_post },
        { .uri = "/video",  .method = HTTP_POST, .handler = video_post },
        { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}
