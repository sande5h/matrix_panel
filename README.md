# matrix_panel

HUB75 LED matrix driver for the ESP32-S3, ESP-IDF v6. Targets a 128x64
1/32-scan panel.

The panel is refreshed by the **PARLIO TX** peripheral running a single looped
DMA transfer. Every HUB75 signal except the pixel clock is a PARLIO data line;
CLK is the peripheral's own clock output. Once started, the CPU is idle unless
a pixel changes -- there is no refresh task and no per-row interrupt.

## Wiring

| HUB75 | GPIO | | HUB75 | GPIO |
|-------|------|-|-------|------|
| R1    | 4    | | A     | 1    |
| G1    | 9    | | B     | 12   |
| B1    | 5    | | C     | 2    |
| R2    | 6    | | D     | 13   |
| G2    | 10   | | E     | 11   |
| B2    | 7    | | LAT   | 14   |
|       |      | | OE    | 21   |
|       |      | | CLK   | 47   |

Ground the panel to the S3 as well, and power the panel from its own 5 V supply
-- a 128x64 panel at full white pulls the better part of 10 A and must not be
fed from the dev board.

The S3's 3.3 V outputs are below the 5 V panel's guaranteed input threshold.
Most panels work anyway on a short cable; if yours is flaky, put a 74AHCT245
level shifter in the ribbon.

## Configuration

Defaults are a 128x64 1/32-scan panel (128 x 64 = 8192 pixels = 32 rows x two
halves x 128 columns, which is what the A..E address lines imply) at 12 MHz.
All of it is in `components/hub75/include/hub75.h`:

- `HUB75_WIDTH` / `HUB75_HEIGHT` -- panel size
- `HUB75_PLANES` -- BCM depth, 6 gives 64 levels per channel
- `HUB75_PCLK_HZ` -- pixel clock; lower it if you see ghosting

At the defaults the refresh buffer is 50 KB of internal DMA RAM and the panel
refreshes at ~473 Hz.

## How the buffer is laid out

The whole frame is one flat array of 16 bit words, one word per pixel clock.
It is split into blocks of `WIDTH + 4` words, one block per (row, bit plane)
pair: `WIDTH` words of pixel data, then a four word blanking tail that pulses
LAT and steps the address lines.

The panel displays whatever was latched at the end of the *previous* block, so
inside block `k` the address lines and the OE window belong to block `k-1`.
That one-block skew is what lets the entire refresh be a static buffer.

Bit plane `p` is lit for `2^p` clocks, scaled so the top plane fills the whole
shift window: 128, 64, 32, 16, 8, 4 clocks. Every OE window therefore hides
inside the data shift and the buffer needs no padding words.

Brightness shortens the OE windows (`hub75_set_brightness`), so it costs no
colour depth at the top of the range and crushes it at the bottom.

## API

```c
hub75_init();                              // allocate + configure
hub75_start();                             // begin the looped DMA refresh
hub75_set_pixel(x, y, r, g, b);            // 8 bit per channel, gamma corrected
hub75_fill(r, g, b);
hub75_clear();
hub75_set_brightness(160);                 // 0..255
```

Drawing writes straight into the live DMA buffer -- there is no double buffer,
so a slow full-frame redraw can tear. A full 128x64 redraw is well under a
millisecond, which fits inside one refresh period.

## Build

```
idf.py set-target esp32s3
idf.py build flash monitor
```

`main.c` runs a red/green/blue/white smoke test and then an animated plasma.

## Status

Written against the ESP-IDF v6 PARLIO TX API; not yet verified on hardware.
Two things to check first on a real panel:

1. **Bit order.** The driver assumes `PARLIO_BIT_PACK_ORDER_LSB` puts bit 0 of
   each word on `data_gpio_nums[0]`. If colours come out scrambled, that
   assumption is where to look.
2. **Latch polarity / tail length.** Four blanking words is generous for most
   panels but some FM6126A-based ones need an init sequence before they will
   display anything at all.
