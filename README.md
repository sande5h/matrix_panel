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

At the defaults the refresh buffer is 48 KiB of internal DMA RAM (192 blocks of
128 words) and the panel refreshes at ~488 Hz.

## How the buffer is laid out

The whole frame is one flat array of 16 bit words, one word per pixel clock.
It is split into blocks of exactly `WIDTH` words, one block per (row, bit
plane) pair. There is no blanking tail: the shift register advances on every
clock whether or not LAT is asserted, so any word appended after the row data
shifts the image sideways by that many columns. LAT is asserted on the last
data word instead, and the OE guard sits at the start of the next block.

The panel displays whatever was latched at the end of the *previous* block, so
inside block `k` the address lines and the OE window belong to block `k-1`.
That one-block skew is what lets the entire refresh be a static buffer.

Bit plane `p` is lit for `2^p` clocks, scaled so the top plane fills the usable
part of the shift window: 125, 62, 31, 15, 7, 3 clocks. Every OE window
therefore hides inside the data shift and the buffer needs no padding words.

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
hub75_blit_rgb565(frame);                  // whole frame, packed 565
hub75_blit_rgb888(frame);                  // whole frame, 3 bytes/pixel
```

Prefer `hub75_blit_rgb888` where the source already has 8 bit channels. RGB565
costs red and blue three bits each *before* the gamma table sees them, which
after gamma leaves those channels with 27 distinct levels against green's 46 --
33,534 colours instead of 262,144. The ROM JPEG decoder emits RGB888 anyway.

Drawing writes straight into the live DMA buffer -- there is no double buffer,
so a slow full-frame redraw can tear. A full 128x64 redraw is well under a
millisecond, which fits inside one refresh period.

## Playing video

Frames live in a dedicated `video` partition -- no filesystem, no codec on the
device beyond the ROM JPEG decoder. Encode a clip you have the rights to:

```
./tools/encode_video.py clip.mp4 video.bin --fps 30 --mjpeg
tools/push.sh video video.bin            # over wifi, no cable
```

Or over USB, which is the only route before the first flash:

```
parttool.py --port /dev/cu.usbmodem* write_partition \
    --partition-name video --input video.bin
```

The script scales and letterboxes to 128x64, converts to RGB565 and prepends a
16 byte header. On boot the app plays whatever is in that partition and falls
back to the plasma if the partition is empty.

Size is the real constraint, because the frames are uncompressed:

| | |
|---|---|
| One raw frame | 16 KiB |
| One second raw at 15 fps | 240 KiB |
| The 12.9M partition, raw | ~55 seconds |
| The 12.9M partition, `--mjpeg` | ~4.5 minutes at 30 fps |

`--mjpeg` stores each frame as a JPEG and decodes it with the ROM tjpgd, which
is roughly twenty times smaller for the same clip and holds 60 fps. Raw is
there for the cases where decode time matters more than space.

`--chroma` selects 420, 422 or 444, and **422 is the default**: compared at
equal bytes per frame it is more accurate than both 420 (which costs 22% more
bytes for 27% more error) and 444 (whose full vertical chroma costs more than
it returns).

Frames are compressed by **cjpeg**, not by ffmpeg, which is a hard dependency
(`brew install jpeg-turbo`). ffmpeg writes 4:2:2 and 4:4:4 with chroma sampling
factors of `0x12`; the ESP32 ROM tjpgd accepts a luma factor of `0x11`, `0x21`
or `0x22` but requires both chroma factors to be exactly `0x11`, so those clips
decode to nothing but `JDR_FMT3` -- a log full of `did not decode (tjpgd 8)`
and a frozen panel. libjpeg writes the canonical factors and all three modes
work.

`--quality` is therefore libjpeg's **0..100** scale, higher being better -- not
ffmpeg's old `-q:v`, where 2 was best and 31 worst. The script warns if you
pass a value low enough to look like the old scale.

Chroma costs decode time, so check the player's `ms/frame` heartbeat before
pairing it with 60 fps: the budget there is 16.7 ms a frame and 4:2:0 already
used 91% of it. **30 fps at `--chroma 422 --quality 89` looks better than
60 fps at 4:2:0**, and tears half as often.

## Control API

The panel serves a small API on **8088**, and answers to `matrix-panel.local`
over mDNS so a DHCP change does not break anything pointing at it.

| endpoint | does |
|---|---|
| `GET /` | status page with buttons |
| `GET /status` | current screen, IP, uptime and the last quota reading, as JSON |
| `GET`/`POST /toggle` | advance to the next screen |
| `GET /screen?s=clock\|claude\|nowplaying\|video` | select one directly |
| `POST /nowplaying` | JSON: what the Mac is playing |
| `POST /nowplaying/art` | 48x48 RGB565, exactly 4608 bytes |
| `POST /video[?play=1]` | replace the clip, no cable |
| `POST /ota` | replace the firmware, then reboot |
| `POST /reboot` | reboot |

Four screens: **clock**, **claude**, **nowplaying** and **video** (the clip in
the video partition). Video playback checks the current screen once per frame,
so switching away interrupts a clip instead of waiting for it to end.

The clock face is a minute sweep on the top row, the date, the time at 3x, and
the Claude quota as two bare bars along the bottom four rows and the four rows
above the gap. No labels or numbers on those: the length is the reading, and a
percentage is unreadable at that size anyway.

Every endpoint returns the same JSON, so the web page and the menu bar item
can never disagree about what is on screen.

### Menu bar item

`tools/matrix_panel.lua` is a Hammerspoon module: left click toggles, the
dropdown selects a screen directly, and the icon shows what is playing. Install
it with:

```
cp tools/matrix_panel.lua ~/.hammerspoon/
cp -r tools/icons ~/.hammerspoon/
# then add to ~/.hammerspoon/init.lua:
#   pcall(require, "matrix_panel")
```

The icons are loaded as template images, so macOS tints them to match the menu
bar and they invert properly in dark mode. If one fails to load the item falls
back to a text glyph rather than going invisible. Artwork from icons8, whose
free tier asks for a link back.

It polls `/status` every 30 s, so the icon tracks changes made from the web
page or after a panel reboot.

The dropdown also uploads:

- **Upload video...** takes a `.bin` from the encoder, or *any* video file --
  an mp4 is encoded first with `--fps 30 --mjpeg --fit crop` and then pushed.
  Reach for the command line when a clip needs `--rotate` or `--focus`.
- **Flash firmware...** posts `build/matrix_panel.bin`, after a confirmation,
  because it reboots the panel.

curl does the transfer, so a 3 MB image streams from disk instead of going
through Lua, and its progress meter is parsed back out of stderr to drive the
menu bar title. The panel draws its own progress bar at the same time. Both
pollers pause during an upload -- the panel serves one request at a time, so a
status poll would just queue behind several megabytes.

## Updating over wifi

Both the firmware and the clip can be replaced without touching the panel.

```
idf.py build
tools/push.sh fw                  # POST build/matrix_panel.bin to /ota
tools/push.sh video demon.bin     # POST a clip to /video, and play it
```

The same two uploads are on the web page at `http://matrix-panel.local:8088/`,
with a progress bar, which is the easier route from a phone.

While an upload runs the panel shows its own progress screen -- a percentage
and a bar -- and goes back to whatever it was showing when it finishes. A
failure stays up for five seconds with the reason, so a push that dies halfway
is not silently invisible.

Neither upload can leave the panel unbootable:

- **Firmware** is written to whichever app slot is *not* running, verified by
  `esp_ota_end()` before it is made bootable, and the reply is sent before the
  reboot. A dropped connection leaves the running slot untouched. If the new
  image boots but cannot get as far as starting its HTTP server, the bootloader
  rolls back to the previous slot on the next reset
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), so a bad push costs a power cycle
  rather than a walk over with a cable.
- **Video** holds the 16 byte header back and writes it last, so an interrupted
  upload leaves an unplayable partition rather than one that plays garbage. The
  header is validated -- magic, 128x64, non-zero fps -- before anything is
  erased, so pushing the wrong file is a 400 with the old clip still intact.
  The player is stopped and waited out first; it never reads flash that is
  being erased underneath it.

> The partition table changed to make room for two app slots, so the **first**
> build after this needs a full USB flash (`idf.py flash`) to rewrite the
> table. Every one after that can go over wifi. Rewriting the table also wipes
> the video partition, so re-push the clip afterwards.

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
idf.py build flash monitor        # USB, needed once for the partition table
tools/push.sh fw                  # every time after that
```

`PIN_CHECK` and `SELF_TEST` at the top of `main.c` turn on the wiring checks;
both are off by default.

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
