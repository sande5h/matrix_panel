# matrix_panel

HUB75 LED matrix driver for the ESP32-S3, ESP-IDF v6. Targets a 128x64
1/32-scan panel.

The panel is refreshed by the **LCD_CAM** peripheral's i80 bus. Every HUB75
signal is one lane of the 16 bit parallel bus, CLK is the bus WR strobe, and
GDMA pushes the entire frame in one transaction. LCD_CAM has no hardware loop
mode, so a small task re-queues the frame forever with a queue depth of two --
one transfer runs while the next is already pending, and the task sits blocked
the rest of the time. The cost is one interrupt per frame, not per row.

(The S3 has no PARLIO peripheral -- LCD_CAM is the only 16-bit parallel DMA
engine on this chip, and it is what the well-known Arduino HUB75 DMA library
uses too.)

## Wiring

| HUB75 | GPIO | | HUB75 | GPIO |
|-------|------|-|-------|------|
| R1    | 8    | | A     | 1    |
| G1    | 9    | | B     | 12   |
| B1    | 5    | | C     | 2    |
| R2    | 6    | | D     | 13   |
| G2    | 10   | | E     | 11   |
| B2    | 7    | | LAT   | 14   |
|       |      | | OE    | 21   |
|       |      | | CLK   | 47   |

The i80 driver requires a D/C pin, and a 16 bit bus requires all 16 data lanes
to be real GPIOs -- it rejects `-1`. HUB75 uses only 13 of them, so **GPIO 15,
16, 17 and 18** are burnt as dummies. Leave all four unconnected, or point the
`PIN_DUMMY_*` defines in `hub75.c` at whatever your board has spare.

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

## Playing video

Frames are stored raw in a dedicated `video` partition -- no filesystem, no
decoder on the device. Encode a clip you have the rights to:

```
./tools/encode_video.py clip.mp4 video.bin --fps 15
parttool.py --port /dev/cu.usbmodem* write_partition \
    --partition-name video --input video.bin
```

The script scales and letterboxes to 128x64, converts to RGB565 and prepends a
16 byte header. On boot the app plays whatever is in that partition and falls
back to the plasma if the partition is empty.

Size is the real constraint, because the frames are uncompressed:

| | |
|---|---|
| One frame | 16 KiB |
| One second at 15 fps | 240 KiB |
| The 5M partition | ~21 seconds |

On a 16 MB module the partition can grow to about 13M in `partitions.csv`,
which is roughly a minute. Longer than that wants an SD card or a codec.

`main/video.c` reads one frame at a time into a 16 KiB buffer and calls
`hub75_blit_rgb565()`, which walks each (row, plane) block once rather than
doing six read-modify-writes per pixel.

## Control API

The panel serves a small API on **8088**, and answers to `matrix-panel.local`
over mDNS so a DHCP change does not break anything pointing at it.

| endpoint | does |
|---|---|
| `GET /` | status page with buttons |
| `GET /status` | current screen, IP, uptime and the last quota reading, as JSON |
| `GET`/`POST /toggle` | advance to the next screen |
| `GET /screen?s=clock\|video` | select one directly |

Two screens: **clock** and **video** (the clip in the video partition). Video
playback checks the current screen once per frame, so switching away interrupts
a clip instead of waiting for it to end.

The clock face is a minute sweep on the top row, the date, the time at 3x, and
the Claude quota as two bare bars along the bottom four rows and the four rows
above the gap. No labels or numbers on those: the length is the reading, and a
percentage is unreadable at that size anyway.

Every endpoint returns the same JSON, so the web page and the menu bar item
can never disagree about what is on screen.

### Menu bar item

`tools/matrix_panel.lua` is a Hammerspoon module: left click toggles, the
dropdown selects a screen directly, and the icon shows what is playing
(🕒 clock, 🎞 video, ▪️ unreachable). Install it with:

```
cp tools/matrix_panel.lua ~/.hammerspoon/
# then add to ~/.hammerspoon/init.lua:
#   pcall(require, "matrix_panel")
```

It polls `/status` every 30 s, so the icon tracks changes made from the web
page or after a panel reboot.

## Claude quota

The two bars come from the `ccusage_server.py` that already serves the
`claude_usage_c3` trinket: it reads Claude Code's OAuth token and calls
`api.anthropic.com/api/oauth/usage`, caching for 60 s. The panel is just a
second client of that server, so the token never leaves the PC and the panel
only ever sees percentages.

Point `USAGE_URL` in `main/secrets.h` at whichever address of the PC shares a
subnet with the panel. Without the server running the clock still works and the
quota rows stay blank.

## Build

```
idf.py set-target esp32s3
idf.py build flash monitor
```

`main.c` runs a red/green/blue/white smoke test and then an animated plasma.

## Status

Working on hardware: a Q2.5AB32V4 128x64 P2.5 panel driven by an ESP32-S3 at
12 MHz, ~488 Hz refresh.

Notes from bringing it up:

- **Check `CONFIG_ESPTOOLPY_FLASHSIZE` against your module.** The boot log
  prints the detected size and warns if the image header disagrees.
- A whole colour channel dead in one half of the panel is one wire: the top
  half is R1/G1/B1, the bottom half R2/G2/B2. Drive that half solid in that
  colour and the data pin sits at a steady level a multimeter can read -- 3.3 V
  if the pin is driving, and 8 kOhm to ground at a healthy panel input. That
  found a damaged GPIO here, not a wiring fault.

- On a HUB75E connector the latch is labelled **LE**, not LAT. Same signal.
- Full white at brightness 255 flashes on a modest supply -- that is the panel
  drawing several amps, not a driver fault. 128 is a comfortable default.
- If row content ever ghosts onto another address, raise `HUB75_OE_GUARD` or
  lower `HUB75_PCLK_HZ` before suspecting anything else.
