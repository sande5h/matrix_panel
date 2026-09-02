#include <stdio.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "server.h"
#include "screen.h"
#include "usage.h"
#include "nowplaying.h"
#include "net.h"
#include "update.h"

static const char *TAG = "server";

#define PORT 8088

static void send_state(httpd_req_t *req)
{
    usage_t u;
    bool have = usage_get(&u);

    char body[640];
    int n = snprintf(body, sizeof(body),
        "{\"screen\":\"%s\",\"screens\":[\"clock\",\"claude\",\"nowplaying\",\"video\"],"
        "\"ip\":\"%s\",\"up_s\":%lld,"
        "\"usage\":{\"valid\":%s,\"stale\":%s,\"session_pct\":%d,\"weekly_pct\":%d,"
        "\"session_resets\":\"%s\",\"weekly_resets\":\"%s\"},\"update\":",
        screen_name(screen_get()), net_ip(),
        (long long)(esp_timer_get_time() / 1000000),
        have ? "true" : "false", (have && u.stale) ? "true" : "false",
        u.session_pct, u.weekly_pct, u.session_resets, u.weekly_resets);

    n += update_json(body + n, sizeof(body) - n);
    n += snprintf(body + n, sizeof(body) - n, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* The menu bar script fetches this from a different origin. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, body, n);
}

static esp_err_t status_get(httpd_req_t *req)
{
    send_state(req);
    return ESP_OK;
}

static esp_err_t toggle_any(httpd_req_t *req)
{
    screen_t s = screen_next();
    ESP_LOGI(TAG, "toggled to %s", screen_name(s));
    send_state(req);
    return ESP_OK;
}

static esp_err_t screen_get_handler(httpd_req_t *req)
{
    char query[64], want[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "s", want, sizeof(want)) == ESP_OK) {
        screen_t s;
        if (screen_from_name(want, &s)) {
            screen_set(s);
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown screen");
            return ESP_FAIL;
        }
    }
    send_state(req);
    return ESP_OK;
}

/* The Mac pushes the current track here. Kept small on purpose: the panel
 * stores what it is given and never asks anyone for it. */
static esp_err_t nowplaying_post(httpd_req_t *req)
{
    char buf[512];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
        return ESP_FAIL;
    }

    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n <= 0) return ESP_FAIL;      /* client vanished mid-body */
        got += n;
    }
    buf[got] = '\0';

    if (!nowplaying_update(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_FAIL;
    }
    send_state(req);
    return ESP_OK;
}

/* Raw RGB565, exactly ART_BYTES of it. Scaling and colour conversion happen on
 * the Mac with ffmpeg, so nothing here has to decode an image. */
static esp_err_t art_post(httpd_req_t *req)
{
    if (req->content_len != ART_BYTES) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected 48x48 rgb565");
        return ESP_FAIL;
    }

    static uint8_t buf[ART_BYTES];
    int got = 0;
    while (got < (int)ART_BYTES) {
        int n = httpd_req_recv(req, (char *)buf + got, ART_BYTES - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }

    if (!nowplaying_set_art(buf, ART_BYTES)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad art");
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* Deliberately small: this is a control panel, not a dashboard. It reads the
 * same JSON the menu bar script does, so the two can never disagree, and it
 * posts the two upload endpoints as raw bodies -- no multipart parser on the
 * device, and the browser supplies Content-Length for free. */
static const char PAGE[] =
"<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>matrix panel</title>"
"<style>body{font:15px system-ui;background:#111;color:#eee;margin:0;padding:24px;"
"max-width:640px}button{font:inherit;padding:10px 18px;margin:4px 6px 4px 0;"
"border:1px solid #444;border-radius:8px;background:#1c1c1c;color:#eee;cursor:pointer}"
"button:hover{background:#2a2a2a}button:disabled{opacity:.4;cursor:default}"
"b{color:#ffaa28}h3{margin:24px 0 8px;font-size:14px;color:#888;font-weight:600;"
"text-transform:uppercase;letter-spacing:.08em}"
"input[type=file]{font:inherit;color:#aaa;margin:4px 0}"
"progress{width:100%;height:8px;margin:8px 0}pre{color:#888;white-space:pre-wrap}</style>"
"<h2>matrix panel</h2><p>showing <b id=s>...</b></p>"
"<p><button onclick=\"go('/toggle')\">toggle</button>"
"<button onclick=\"go('/screen?s=clock')\">clock</button>"
"<button onclick=\"go('/screen?s=claude')\">claude</button>"
"<button onclick=\"go('/screen?s=nowplaying')\">playing</button>"
"<button onclick=\"go('/screen?s=video')\">video</button></p>"

"<h3>video</h3><p>a .bin from tools/encode_video.py</p>"
"<input type=file id=vf accept='.bin'>"
"<button id=vb onclick=\"up('/video?play=1','vf','vb','vp')\">upload &amp; play</button>"
"<progress id=vp value=0 max=100 hidden></progress>"

"<h3>firmware</h3><p>build/matrix_panel.bin &mdash; the panel reboots into it</p>"
"<input type=file id=of accept='.bin'>"
"<button id=ob onclick=\"up('/ota','of','ob','op')\">flash</button>"
"<progress id=op value=0 max=100 hidden></progress>"

"<pre id=j></pre>"
"<script>"
"function show(d){document.getElementById('s').textContent=d.screen;"
"document.getElementById('j').textContent=JSON.stringify(d,null,2)}"
"function go(u){fetch(u).then(r=>r.json()).then(show)}"
/* XHR rather than fetch: upload progress events are the whole point, and
   fetch still has no portable equivalent. */
"function up(url,fi,bi,pi){var f=document.getElementById(fi).files[0];if(!f)return;"
"var b=document.getElementById(bi),p=document.getElementById(pi);"
"b.disabled=true;p.hidden=false;p.value=0;"
"var x=new XMLHttpRequest();x.open('POST',url);"
"x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded*100/e.total};"
"x.onload=function(){b.disabled=false;p.hidden=true;"
"document.getElementById('j').textContent=x.responseText;"
"setTimeout(function(){go('/status')},1500)};"
"x.onerror=function(){b.disabled=false;p.hidden=true;"
"document.getElementById('j').textContent='upload failed'};"
"x.send(f)}"
"go('/status')</script>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
    return ESP_OK;
}

esp_err_t server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = PORT;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    /* A firmware or video push is one long POST. The default 5 s is enough per
     * recv, but an erase between chunks and a busy panel can eat into it. */
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;
    /* esp_ota_write and esp_partition_write both recurse a fair way down. */
    cfg.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = root_get },
        { .uri = "/status", .method = HTTP_GET,  .handler = status_get },
        { .uri = "/toggle", .method = HTTP_GET,  .handler = toggle_any },
        { .uri = "/toggle", .method = HTTP_POST, .handler = toggle_any },
        { .uri = "/screen", .method = HTTP_GET,  .handler = screen_get_handler },
        { .uri = "/nowplaying", .method = HTTP_POST, .handler = nowplaying_post },
        { .uri = "/nowplaying/art", .method = HTTP_POST, .handler = art_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    update_register(server);

    ESP_LOGI(TAG, "control API on http://%s:%d/", net_ip(), PORT);
    return ESP_OK;
}
