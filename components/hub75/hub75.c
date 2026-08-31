#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_i80.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"

#include "hub75.h"

static const char *TAG = "hub75";

/* ------------------------------------------------------------------ pins */
/* Every HUB75 signal is one lane of the 16 bit i80 bus except CLK, which is
 * the bus's own WR strobe. The BIT_* value is the lane index, i.e. bit n of
 * each 16 bit word drives data_gpio_nums[n]. */
#define PIN_R1  4
#define PIN_G1  9
#define PIN_B1  5
#define PIN_R2  6
#define PIN_G2 10
#define PIN_B2  7
#define PIN_A   1
#define PIN_B  12
#define PIN_C   2
#define PIN_D  13
#define PIN_E  11
#define PIN_LAT 14
#define PIN_OE  21
#define PIN_CLK 47   /* i80 WR */

/* HUB75 needs 13 of the bus's 16 lanes, but the i80 driver validates all 16
 * and rejects -1, and it wants a D/C pin that HUB75 does not have either. So
 * four pins are burnt as dummies. Leave all four unconnected; any free GPIO
 * works. Avoid 19/20 (USB), 26..32 (SPI flash), 33..37 (octal PSRAM on -R8
 * modules) and 43/44 (console UART). */
#define PIN_DUMMY_DC 15
#define PIN_DUMMY_13 16
#define PIN_DUMMY_14 17
#define PIN_DUMMY_15 18

#define BIT_R1  0
#define BIT_G1  1
#define BIT_B1  2
#define BIT_R2  3
#define BIT_G2  4
#define BIT_B2  5
#define BIT_A   6
#define BIT_B   7
#define BIT_C   8
#define BIT_D   9
#define BIT_E  10
#define BIT_LAT 11
#define BIT_OE 12

#define MASK_RGB  (0x3F << BIT_R1)          /* bits 0..5  */
#define MASK_ADDR (0x1F << BIT_A)           /* bits 6..10 */
#define MASK_LAT  (1u << BIT_LAT)
#define MASK_OE   (1u << BIT_OE)            /* active low: 1 = panel blanked */

/* --------------------------------------------------------------- layout */
/* One block = one (row, plane) pair, and it is exactly WIDTH clocks long.
 * Every word in the stream is a clock, and the panel's shift register moves on
 * every clock whether or not LAT is asserted -- so there is no room for extra
 * "blanking" words after the data. Anything appended shows up as the image
 * sliding sideways by that many columns. LAT is therefore asserted on the last
 * data word, and the address change and OE guard live inside the same WIDTH
 * clocks.
 *
 * The panel always displays what was latched at the end of the *previous*
 * block, so within block k the address lines and the OE window belong to
 * block k-1. That one-block skew is what makes the whole frame a single flat
 * buffer the DMA can push with no CPU involvement. */

/* Words at the start of a block kept dark, covering the latch and the address
 * lines settling. Tunable via HUB75_OE_GUARD. */
#define OE_GUARD    HUB75_OE_GUARD
/* Words at the end of a block carrying the LAT pulse. A few panels want 2. */
#define LAT_WORDS   1

#define BLOCK_WORDS  HUB75_WIDTH
#define NUM_BLOCKS   (HUB75_ROWS * HUB75_PLANES)
#define BUF_WORDS    (NUM_BLOCKS * BLOCK_WORDS)
#define BUF_BYTES    (BUF_WORDS * sizeof(uint16_t))

#define BLOCK_OF(row, plane) ((row) * HUB75_PLANES + (plane))
#define BLOCK_BASE(k)        ((k) * BLOCK_WORDS)

/* GDMA wants the transfer size burst aligned. With the stock geometry this
 * works out exactly; if you change WIDTH or PLANES and trip this, pad the
 * block until it divides again. */
_Static_assert(BUF_BYTES % 64 == 0, "refresh buffer must be a multiple of the 64 byte DMA burst");

/* Clocks available for the OE window, once the guard and the latch are taken
 * out of the row. */
#define OE_SPAN (HUB75_WIDTH - OE_GUARD - LAT_WORDS)

/* Plane p is lit for 2^p clocks scaled so the MSB plane fills the usable part
 * of the row shift window. All of it hides inside the data shift, so the
 * buffer needs no padding words. */
static inline int plane_weight(int plane)
{
    return (OE_SPAN << plane) >> (HUB75_PLANES - 1);
}

/* --------------------------------------------------------------- state */
static uint16_t *s_buf;
static esp_lcd_i80_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static TaskHandle_t s_task;
static volatile uint32_t s_frames;
static uint8_t s_brightness = 160;
static volatile bool s_running;
static uint8_t s_gamma[256];

static void build_gamma(void)
{
    const int maxv = (1 << HUB75_PLANES) - 1;
    for (int i = 0; i < 256; i++) {
        /* lrintf already rounds; adding 0.5 on top pushed 255 to 64, which
         * does not fit in HUB75_PLANES bits -- every plane bit came out zero
         * and full-brightness channels rendered as black. Clamp regardless. */
        int v = (int)lrintf(powf(i / 255.0f, 2.2f) * maxv);
        s_gamma[i] = (uint8_t)(v > maxv ? maxv : v);
    }
}

/* Rewrites the OE bit of every word. Safe to call while refreshing -- the
 * worst case is one visibly dim frame. */
static void apply_oe(void)
{
    for (int k = 0; k < NUM_BLOCKS; k++) {
        int prev  = (k + NUM_BLOCKS - 1) % NUM_BLOCKS;
        int plane = prev % HUB75_PLANES;

        int on = plane_weight(plane) * s_brightness / 255;
        if (on > OE_SPAN) on = OE_SPAN;

        uint16_t *w = &s_buf[BLOCK_BASE(k)];
        for (int i = 0; i < BLOCK_WORDS; i++) {
            bool lit = (i >= OE_GUARD) && (i < OE_GUARD + on);
            if (lit) w[i] &= (uint16_t)~MASK_OE;   /* enabled */
            else     w[i] |=  MASK_OE;             /* blanked */
        }
    }
}

/* Lays down everything that never changes: address lines, LAT pulse, OE. */
static void build_skeleton(void)
{
    memset(s_buf, 0, BUF_BYTES);

    for (int k = 0; k < NUM_BLOCKS; k++) {
        int prev     = (k + NUM_BLOCKS - 1) % NUM_BLOCKS;
        int addr_now = prev / HUB75_PLANES;   /* the row currently displayed */

        uint16_t *w = &s_buf[BLOCK_BASE(k)];

        for (int i = 0; i < BLOCK_WORDS; i++) {
            w[i] = (uint16_t)(addr_now << BIT_A);
        }
        /* Latch the row we just shifted, on the last clock of the block. The
         * address moves to the new row at the start of the next block, while
         * OE_GUARD keeps the panel dark. */
        for (int i = BLOCK_WORDS - LAT_WORDS; i < BLOCK_WORDS; i++) {
            w[i] |= MASK_LAT;
        }
    }

    apply_oe();
}

static bool IRAM_ATTR on_frame_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    s_frames++;
    return false;
}

/* The LCD peripheral has no hardware loop mode, so the frame is re-queued
 * forever. With a queue depth of two, one transfer is always pending while
 * another runs and the task spends its life blocked inside tx_color. */
static void refresh_task(void *arg)
{
    while (s_running) {
        esp_err_t err = esp_lcd_panel_io_tx_color(s_io, -1, s_buf, BUF_BYTES);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tx_color: %s", esp_err_to_name(err));
            break;
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- API */
esp_err_t hub75_init(void)
{
    ESP_RETURN_ON_FALSE(!s_buf, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    esp_err_t ret = ESP_OK;   /* ESP_GOTO_ON_* report through this */

    build_gamma();

    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src            = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num        = PIN_DUMMY_DC,
        .wr_gpio_num        = PIN_CLK,
        .bus_width          = 16,
        .max_transfer_bytes = BUF_BYTES,
        .dma_burst_size     = 64,
        .data_gpio_nums = {
            [BIT_R1] = PIN_R1, [BIT_G1] = PIN_G1, [BIT_B1] = PIN_B1,
            [BIT_R2] = PIN_R2, [BIT_G2] = PIN_G2, [BIT_B2] = PIN_B2,
            [BIT_A]  = PIN_A,  [BIT_B]  = PIN_B,  [BIT_C]  = PIN_C,
            [BIT_D]  = PIN_D,  [BIT_E]  = PIN_E,
            [BIT_LAT] = PIN_LAT,
            [BIT_OE]  = PIN_OE,
            [13] = PIN_DUMMY_13, [14] = PIN_DUMMY_14, [15] = PIN_DUMMY_15,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &s_bus), TAG, "i80 bus failed");

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num        = -1,            /* exclusive use of the bus */
        .pclk_hz            = HUB75_PCLK_HZ,
        .trans_queue_depth  = 2,
        .lcd_cmd_bits       = 0,             /* raw data, no command phase */
        .lcd_param_bits     = 0,
        .on_color_trans_done = on_frame_done,
        .flags = {
            .pclk_idle_low = true,           /* CLK rests low between frames */
        },
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i80(s_bus, &io_cfg, &s_io), err, TAG, "i80 io failed");

    /* Let the driver pick the alignment GDMA and the cache want. */
    s_buf = esp_lcd_i80_alloc_draw_buffer(s_io, BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_GOTO_ON_FALSE(s_buf, ESP_ERR_NO_MEM, err, TAG,
                      "no DMA memory for %u byte refresh buffer", (unsigned)BUF_BYTES);
    build_skeleton();

    ESP_LOGI(TAG, "%dx%d, 1/%d scan, %d planes, %.1f MHz pclk, %u byte buffer, ~%.0f Hz refresh",
             HUB75_WIDTH, HUB75_HEIGHT, HUB75_ROWS, HUB75_PLANES,
             HUB75_PCLK_HZ / 1e6f, (unsigned)BUF_BYTES,
             (float)HUB75_PCLK_HZ / (float)BUF_WORDS);
    return ESP_OK;

err:
    if (s_io)  { esp_lcd_panel_io_del(s_io); s_io = NULL; }
    if (s_bus) { esp_lcd_del_i80_bus(s_bus); s_bus = NULL; }
    return ret;
}

esp_err_t hub75_start(void)
{
    ESP_RETURN_ON_FALSE(s_buf && !s_running, ESP_ERR_INVALID_STATE, TAG, "not ready");

    s_running = true;
    /* Pinned to core 1 and above the default priority so a busy app can't
     * stall the re-queue and blink the panel. */
    if (xTaskCreatePinnedToCore(refresh_task, "hub75", 3072, NULL, 10, &s_task, 1) != pdPASS) {
        s_running = false;
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "refresh task failed");
    }
    return ESP_OK;
}

esp_err_t hub75_stop(void)
{
    ESP_RETURN_ON_FALSE(s_running, ESP_ERR_INVALID_STATE, TAG, "not running");
    s_running = false;
    /* The task exits after its in-flight frame drains. */
    while (s_task) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_OK;
}

void hub75_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_buf || (unsigned)x >= HUB75_WIDTH || (unsigned)y >= HUB75_HEIGHT) return;

    int row = y % HUB75_ROWS;
    bool lower = y >= HUB75_ROWS;

    uint16_t rm = lower ? (1u << BIT_R2) : (1u << BIT_R1);
    uint16_t gm = lower ? (1u << BIT_G2) : (1u << BIT_G1);
    uint16_t bm = lower ? (1u << BIT_B2) : (1u << BIT_B1);

    uint8_t rv = s_gamma[r], gv = s_gamma[g], bv = s_gamma[b];

    for (int p = 0; p < HUB75_PLANES; p++) {
        uint16_t *w = &s_buf[BLOCK_BASE(BLOCK_OF(row, p)) + x];
        uint16_t v = *w & (uint16_t)~(rm | gm | bm);
        if (rv & (1 << p)) v |= rm;
        if (gv & (1 << p)) v |= gm;
        if (bv & (1 << p)) v |= bm;
        *w = v;
    }
}

void hub75_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = 0; y < HUB75_HEIGHT; y++) {
        for (int x = 0; x < HUB75_WIDTH; x++) {
            hub75_set_pixel(x, y, r, g, b);
        }
    }
}

void hub75_clear(void)
{
    if (!s_buf) return;
    /* Faster than walking pixels: just knock out the six colour bits. */
    for (int k = 0; k < NUM_BLOCKS; k++) {
        uint16_t *w = &s_buf[BLOCK_BASE(k)];
        for (int i = 0; i < BLOCK_WORDS; i++) w[i] &= (uint16_t)~MASK_RGB;
    }
}

void hub75_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
    if (s_buf) apply_oe();
}

uint8_t hub75_get_brightness(void)
{
    return s_brightness;
}

uint32_t hub75_frame_count(void)
{
    return s_frames;
}

float hub75_refresh_hz(void)
{
    static int64_t last_us;
    static uint32_t last_frames;

    int64_t now = esp_timer_get_time();
    uint32_t frames = s_frames;

    float hz = 0.0f;
    if (last_us && now > last_us) {
        hz = (frames - last_frames) * 1e6f / (float)(now - last_us);
    }
    last_us = now;
    last_frames = frames;
    return hz;
}
