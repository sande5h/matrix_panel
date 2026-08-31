#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/parlio_tx.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"

#include "hub75.h"

static const char *TAG = "hub75";

/* ------------------------------------------------------------------ pins */
/* Every HUB75 signal is a PARLIO data line except CLK, which is the
 * peripheral's own clock output. The bit position below is the PARLIO data
 * line index, i.e. bit n of each 16 bit word drives data_gpio_nums[n]. */
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
#define PIN_CLK 47

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
/* One block = one (row, plane) pair: WIDTH clocks of pixel data followed by a
 * short blanking tail that pulses LAT and moves the address lines on.
 *
 * The panel always displays what was latched at the end of the *previous*
 * block, so within block k the address lines and the OE window belong to
 * block k-1. That one-block skew is what makes the whole thing a single flat
 * buffer with no CPU involvement. */
#define BLANK_WORDS  4
#define BLOCK_WORDS  (HUB75_WIDTH + BLANK_WORDS)
#define NUM_BLOCKS   (HUB75_ROWS * HUB75_PLANES)
#define BUF_WORDS    (NUM_BLOCKS * BLOCK_WORDS)
#define BUF_BYTES    (BUF_WORDS * sizeof(uint16_t))

#define BLOCK_OF(row, plane) ((row) * HUB75_PLANES + (plane))
#define BLOCK_BASE(k)        ((k) * BLOCK_WORDS)

/* Plane p is lit for 2^p clocks scaled so the MSB plane fills the whole row
 * shift window. With WIDTH 64 and 6 planes that is 64, 32, 16, 8, 4, 2 -- all
 * of it hidden inside the data shift, so no padding words are needed. */
static inline int plane_weight(int plane)
{
    return (HUB75_WIDTH << plane) >> (HUB75_PLANES - 1);
}

/* --------------------------------------------------------------- state */
static uint16_t *s_buf;
static parlio_tx_unit_handle_t s_tx;
static uint8_t s_brightness = 160;
static bool s_running;
static uint8_t s_gamma[256];

static void build_gamma(void)
{
    const int maxv = (1 << HUB75_PLANES) - 1;
    for (int i = 0; i < 256; i++) {
        s_gamma[i] = (uint8_t)lrintf(powf(i / 255.0f, 2.2f) * maxv + 0.5f);
    }
}

/* Rewrites the OE bit of every word. Safe to call while the DMA loop is
 * running -- the worst case is one visibly dim frame. */
static void apply_oe(void)
{
    for (int k = 0; k < NUM_BLOCKS; k++) {
        int prev  = (k + NUM_BLOCKS - 1) % NUM_BLOCKS;
        int plane = prev % HUB75_PLANES;

        int on = plane_weight(plane) * s_brightness / 255;
        if (on > HUB75_WIDTH) on = HUB75_WIDTH;

        uint16_t *w = &s_buf[BLOCK_BASE(k)];
        for (int i = 0; i < HUB75_WIDTH; i++) {
            if (i < on) w[i] &= (uint16_t)~MASK_OE;   /* enabled  */
            else        w[i] |=  MASK_OE;             /* blanked  */
        }
        /* The blanking tail is always dark: LAT and the address lines only
         * ever move while the panel is off. */
        for (int i = HUB75_WIDTH; i < BLOCK_WORDS; i++) {
            w[i] |= MASK_OE;
        }
    }
}

/* Lays down everything that never changes: address lines, LAT pulses, OE. */
static void build_skeleton(void)
{
    memset(s_buf, 0, BUF_BYTES);

    for (int k = 0; k < NUM_BLOCKS; k++) {
        int prev     = (k + NUM_BLOCKS - 1) % NUM_BLOCKS;
        int addr_now = prev / HUB75_PLANES;          /* row being displayed */
        int addr_new = k / HUB75_PLANES;             /* row being shifted   */

        uint16_t *w = &s_buf[BLOCK_BASE(k)];

        for (int i = 0; i < HUB75_WIDTH; i++) {
            w[i] = (uint16_t)(addr_now << BIT_A);
        }
        /* tail: hold the old address over the latch pulse, then switch. */
        w[HUB75_WIDTH + 0] = (uint16_t)(addr_now << BIT_A) | MASK_LAT;
        w[HUB75_WIDTH + 1] = (uint16_t)(addr_now << BIT_A) | MASK_LAT;
        w[HUB75_WIDTH + 2] = (uint16_t)(addr_new << BIT_A);
        w[HUB75_WIDTH + 3] = (uint16_t)(addr_new << BIT_A);
    }

    apply_oe();
}

/* ---------------------------------------------------------------- API */
esp_err_t hub75_init(void)
{
    ESP_RETURN_ON_FALSE(!s_buf, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    build_gamma();

    /* PARLIO reads this straight out of DMA, so it must be internal RAM. */
    s_buf = heap_caps_aligned_calloc(64, 1, BUF_BYTES,
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_buf, ESP_ERR_NO_MEM, TAG,
                        "no DMA memory for %u byte refresh buffer", (unsigned)BUF_BYTES);
    build_skeleton();

    parlio_tx_unit_config_t cfg = {
        .clk_src           = PARLIO_CLK_SRC_DEFAULT,
        .data_width        = 16,
        .clk_in_gpio_num   = -1,
        .clk_out_gpio_num  = PIN_CLK,
        .valid_gpio_num    = -1,
        .output_clk_freq_hz = HUB75_PCLK_HZ,
        .trans_queue_depth = 2,
        .max_transfer_size = BUF_BYTES,
        .sample_edge       = PARLIO_SAMPLE_EDGE_POS,
        .bit_pack_order    = PARLIO_BIT_PACK_ORDER_LSB,
        .data_gpio_nums = {
            [BIT_R1] = PIN_R1, [BIT_G1] = PIN_G1, [BIT_B1] = PIN_B1,
            [BIT_R2] = PIN_R2, [BIT_G2] = PIN_G2, [BIT_B2] = PIN_B2,
            [BIT_A]  = PIN_A,  [BIT_B]  = PIN_B,  [BIT_C]  = PIN_C,
            [BIT_D]  = PIN_D,  [BIT_E]  = PIN_E,
            [BIT_LAT] = PIN_LAT,
            [BIT_OE]  = PIN_OE,
            [13] = -1, [14] = -1, [15] = -1,
        },
    };

    esp_err_t err = parlio_new_tx_unit(&cfg, &s_tx);
    if (err != ESP_OK) {
        heap_caps_free(s_buf);
        s_buf = NULL;
        ESP_RETURN_ON_ERROR(err, TAG, "parlio_new_tx_unit failed");
    }

    ESP_LOGI(TAG, "%dx%d, 1/%d scan, %d planes, %.1f MHz pclk, %u byte buffer, %.0f Hz refresh",
             HUB75_WIDTH, HUB75_HEIGHT, HUB75_ROWS, HUB75_PLANES,
             HUB75_PCLK_HZ / 1e6f, (unsigned)BUF_BYTES, hub75_refresh_hz());
    return ESP_OK;
}

esp_err_t hub75_start(void)
{
    ESP_RETURN_ON_FALSE(s_buf && !s_running, ESP_ERR_INVALID_STATE, TAG, "not ready");

    ESP_RETURN_ON_ERROR(parlio_tx_unit_enable(s_tx), TAG, "enable failed");

    parlio_transmit_config_t tcfg = {
        .idle_value = MASK_OE,               /* park the panel blanked */
        .flags = { .loop_transmission = true },
    };
    /* Payload length is in bits, not bytes. */
    ESP_RETURN_ON_ERROR(parlio_tx_unit_transmit(s_tx, s_buf, BUF_BYTES * 8, &tcfg),
                        TAG, "transmit failed");

    s_running = true;
    return ESP_OK;
}

esp_err_t hub75_stop(void)
{
    ESP_RETURN_ON_FALSE(s_running, ESP_ERR_INVALID_STATE, TAG, "not running");
    /* Disabling the unit aborts the loop; the idle value blanks the panel. */
    ESP_RETURN_ON_ERROR(parlio_tx_unit_disable(s_tx), TAG, "disable failed");
    s_running = false;
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
        for (int i = 0; i < HUB75_WIDTH; i++) w[i] &= (uint16_t)~MASK_RGB;
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

float hub75_refresh_hz(void)
{
    return (float)HUB75_PCLK_HZ / (float)BUF_WORDS;
}
