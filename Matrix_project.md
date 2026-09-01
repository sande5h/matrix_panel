# Driving a 128x64 HUB75 panel from an ESP32-S3, with no library

*Build log, 31 August - 1 September 2026. 48 commits, one damaged GPIO, one
bug I blamed on the hardware before finding it in my own gamma table.*

---

<!-- PHOTO 01 -->
![The finished panel on the desk, showing the clock face](photos/01-hero.jpg)

> **Shoot:** the panel running the clock face at night, slightly off-axis so
> the individual LEDs read as pixels. Keep the ESP32 and the ribbon cable in
> frame. Shutter **1/60 s or slower** — see the note on photographing this
> thing at the end.

---

## What this is

A 128x64 P2.5 RGB LED matrix (a Q2.5AB32V4, 1/32 scan, HUB75E connector)
driven directly by an ESP32-S3, with the driver written from scratch. It shows
a clock, my Claude usage quota, whatever my browser is playing, and video —
and it takes new firmware and new video clips over Wi-Fi.

The interesting part is not the finished thing. It is that a HUB75 panel has
no controller in it at all.

## Why no library

There is a well-known Arduino library for this. I did not use it, for a
reason that is worth stating plainly: a HUB75 panel is not a display you send
pixels to. It is **six shift registers, five address lines, a latch and an
output enable**, and nothing else. There is no frame buffer on the panel, no
refresh circuit, no controller. If the microcontroller stops feeding it, it
goes dark — or worse, it leaves one row lit and cooks it.

Every visible property of the image — brightness, colour depth, refresh rate,
whether the top half is even the *same picture* as the bottom half — is a
consequence of the exact bit pattern you clock out. That is the whole project.
Wrapping it in a library would have meant never learning where any of those
properties come from, and I would not have been able to diagnose a single one
of the faults below.

The panel's actual interface:

| Signal | What it does |
|---|---|
| `R1 G1 B1` | serial colour data for the **top** half (rows 0–31) |
| `R2 G2 B2` | serial colour data for the **bottom** half (rows 32–63) |
| `A B C D E` | which row pair of 32 is currently lit |
| `CLK` | shifts one pixel into all six registers |
| `LAT` (`LE`) | copies the shift registers to the output drivers |
| `OE` | output enable — **active low** |

One bit per channel per pixel. Not eight. **One.** An LED is on or it is off.

<!-- PHOTO 02 -->
![The HUB75E connector and the ribbon, with the pin labels visible](photos/02-connector.jpg)

> **Shoot:** close-up of the panel's input connector with the silkscreen
> legend readable. This is the photo people actually need — note the latch is
> labelled **LE**, not LAT, which cost me a few minutes of confusion.

---

## Part 1: finding a peripheral fast enough

### The dead end

Espressif has a `parlio_tx` example that drives exactly this kind of matrix.
PARLIO is a parallel I/O peripheral with DMA — perfect. I started there.

It did not compile. `PARLIO_CLK_SRC_DEFAULT` undeclared, arrays out of bounds.

**The ESP32-S3 has no PARLIO peripheral.** It arrived on the C6 and the P4.
The example is real, it just does not target this chip. I had wasted a commit
finding that out (`175f9e2`, later rewritten).

### What actually works

The S3's **LCD_CAM** peripheral has an i80 (Intel 8080) bus mode: 16 data
lanes, a write strobe, and GDMA behind it. That makes it the only 16-bit
parallel DMA engine on this chip, and it is what the Arduino library uses too,
under the name "I2S DMA" — same silicon, older name.

So: **every HUB75 signal becomes one lane of a 16-bit bus, CLK becomes the bus
write strobe, and the entire refresh becomes one DMA transfer.**

```
R1 G1 B1 R2 G2 B2   A B C D E   LAT   OE   |   dummy dummy dummy
 0  1  2  3  4  5   6 7 8 9 10   11   12   |    13    14    15
```

13 signals in a 16-bit bus. That immediately caused the first real bug.

### Bug 1 — `esp_lcd_new_i80_bus` returns `ESP_ERR_INVALID_ARG`

I set the three unused lanes to `-1`, the usual "not connected" convention.
The 16-bit bus validates **all sixteen** GPIOs and rejects the sentinel.

Fix: give them real, unconnected pins (GPIO 15–18) and never wire them. The
i80 driver also demands a D/C pin it will never use — same treatment.

```c
.data_gpio_nums = {
    [BIT_R1] = PIN_R1, ... [BIT_OE] = PIN_OE,
    [13] = PIN_DUMMY_13, [14] = PIN_DUMMY_14, [15] = PIN_DUMMY_15,
},
```

Worth writing down because the error code tells you nothing, and "assign four
GPIOs you will deliberately leave floating" is not an obvious fix.

---

## Part 2: the buffer *is* the refresh

This is the core idea, and once it clicks the rest of the driver is bookkeeping.

The DMA transfer is a flat array of 16-bit words. **Every word is one clock
edge.** The bit pattern in word *n* is the literal state of all 13 HUB75 pins
during clock *n*. There is no "drawing" — there is only deciding what the pins
do, 24,576 clocks at a time, forever.

The array is divided into **blocks**, one block per (row, bit plane) pair:

```
32 rows x 6 bit planes = 192 blocks
192 blocks x 128 words = 24,576 words = 48 KiB of internal DMA RAM
12 MHz / 24,576 words  = ~488 Hz refresh
```

### Colour from one-bit LEDs: binary code modulation

Six planes give 64 levels per channel. Plane *p* is lit for 2^p clocks —
4, 8, 16, 32, 64, 128 — so a channel's brightness is the sum of the planes
where its bit is set. Persistence of vision does the rest.

The elegant part: **the OE window hides entirely inside the data shift.** The
panel has to clock 128 pixels into the row anyway; lighting the previous row
for some prefix of those 128 clocks is free. No padding words, no idle time.

```c
#define OE_SPAN (HUB75_WIDTH - OE_GUARD - LAT_WORDS)

static inline int plane_weight(int plane)
{
    return (OE_SPAN << plane) >> (HUB75_PLANES - 1);
}
```

Brightness control is just shortening those windows — which is why it costs no
colour depth at the top of the range and crushes it at the bottom.

### The one-block skew

Here is the thing that makes it all fit. While block *k* is shifting in row
*k*'s data, the panel is **displaying what was latched at the end of block
*k−1***. So inside block *k*, the address lines and the OE window belong to
the *previous* block.

```c
int prev     = (k + NUM_BLOCKS - 1) % NUM_BLOCKS;
int addr_now = prev / HUB75_PLANES;    /* the row currently displayed */
```

Get this off by one and the panel shows the right pixels on the wrong row, at
the wrong brightness. Get it right and the whole refresh is a **static
buffer** — the CPU touches it only when the image changes. One interrupt per
frame, and it is only there to count frames.

<!-- DIAGRAM 01 -->
![Diagram: one block of 128 words, showing the OE guard, the OE window, and the LAT pulse on the last word](diagrams/block-layout.png)

> **Draw:** a horizontal strip of 128 cells. Mark cells 0–1 as `OE_GUARD`
> (dark), cells 2..2+on as the lit OE window, and the final cell as `LAT`.
> Label the whole strip "address lines = row k−1 throughout".

### The refresh task

LCD_CAM has no hardware loop mode, so a task re-queues the same buffer forever
with a queue depth of two — one transfer running, one already pending, and the
task blocked the rest of the time.

```c
while (s_running) {
    esp_lcd_panel_io_tx_color(s_io, -1, s_buf, BUF_BYTES);
}
```

That is the entire refresh engine. Pinned to core 1, above default priority,
so a busy application task cannot stall it into a visible blink.

---

## Part 3: five bugs, and what each one actually was

This is the part worth reading. Every one of these looked like a wiring fault.
Two were.

<!-- PHOTO 03 -->
![Panel showing the RGB test pattern with the fault visible](photos/03-fault.jpg)

> **Shoot:** if you still have a way to reproduce any of these, photograph it.
> The two-column shift and the stray centre line both photograph well. If not,
> a photo of the test pattern working is still worth having here.

### Bug 2 — the image slid two columns left

Symptom: everything drawn shifted left by exactly two pixels, wrapping around.

My first instinct was a timing problem. It was not.

I had appended two "blanking" words after each row's data to carry the LAT
pulse. But **the shift register advances on every clock, whether or not LAT is
asserted.** Two extra clocks per row means two extra pixels shifted in, so the
image walks left by two. Nothing about it is a timing subtlety — it is exactly
as many columns as I added words.

Fix: no extra words, ever. LAT is asserted **on the last data word**, and the
OE guard lives at the *start* of the next block, inside the same 128 clocks.

> **The rule:** on HUB75, a word is a clock, and a clock is a pixel. If you
> add words you move the picture.

### Bug 3 — every solid colour rendered as black

Solid red, green, blue and white: all black. Gradients and the plasma demo:
fine.

I looked at the pattern — full-brightness channels fail, partial ones work —
and told the user their GPIO 4 wiring was at fault.

**I was wrong, and it was my bug.** The gamma table:

```c
/* lrintf already rounds; adding 0.5 on top pushed 255 to 64, which
 * does not fit in HUB75_PLANES bits -- every plane bit came out zero
 * and full-brightness channels rendered as black. Clamp regardless. */
int v = (int)lrintf(powf(i / 255.0f, 2.2f) * maxv);
s_gamma[i] = (uint8_t)(v > maxv ? maxv : v);
```

`maxv` is 63. I had double-rounded and produced 64 — which is `0b1000000`, one
bit above the six planes. Masking each plane out gave **zero for every plane**.
The brightest possible input produced the darkest possible output, and only
for inputs that rounded to the very top.

Two lessons, and the second one matters more:

1. Clamp a lookup table to its output range even when the maths "obviously"
   cannot exceed it.
2. **Do not accuse the hardware until the software is exonerated.** I sent
   someone to the workbench with a multimeter to check a wire that was fine.
   The symptom — *only* the extreme values fail — was pointing at arithmetic
   the whole time. A broken wire does not care how bright the pixel is.

### Bug 4 — a stray lit line across row 32

A thin line at the boundary between the two halves, in every pattern,
including a supposedly blank frame.

Cause: **OE is active low.** The i80 peripheral inserts blank (all-zero) clocks
between transactions, and an all-zero word means `OE = 0`, which means *lit* —
at whatever address the lines happened to hold, which was 0. The gap between
DMA frames was being displayed.

The fix is neat. Rather than inverting the bit everywhere in software, invert
the pin in the **GPIO matrix** on the way out:

```c
_Static_assert(LCD_DATA_OUT12_IDX == LCD_DATA_OUT0_IDX + 12,
               "LCD data out signals are not contiguous");
esp_rom_gpio_connect_out_signal(PIN_OE, LCD_DATA_OUT0_IDX + BIT_OE,
                                true /* invert */, false);
```

Now zero means blanked, so any gap in the stream is dark by construction, and
in the buffer the bit reads as "lit", which is how you want to think about it.

(Detour: `hal/lcd_periph.h` has the signal table, but including it pulls in
`hal/i2s_ll.h`, which this component does not depend on and which does not
resolve cleanly. `soc/gpio_sig_map.h` has the same constants with no baggage —
hence the `_Static_assert` guarding the assumption that they are contiguous.)

### Bug 5 — a blue ghost row that was not mine

A duplicated row appeared under the bottom-right block. I chased it for six
commits: slowed the pixel clock to 6 MHz, widened the OE guard, swept a single
block across every address to find where the ghost originated.

`c07133a Revert "Slow the pixel clock to 6MHz and widen the OE guard"`
`a1e29cb Restore the demo app now that the ghost was the panel`

**It was the panel.** Changing what was displayed changed which row ghosted —
which is the signature of a driver-IC or a physical fault in the panel, not of
the signal you are sending it. The timing changes I had made to chase it were
costing refresh rate for nothing, so they came out.

Knowing when to stop is a skill. The tell was that the ghost *moved with the
content*, and no amount of guard time touched it.

### Bug 6 — the red that really was a dead GPIO

Top half red: nothing. Bottom half red: fine.

This time it *was* hardware, and the method for proving it is the single most
useful thing in this whole writeup:

> **Drive one half of the panel solid in one colour and hold it.** The data
> pin for that colour then sits at a steady, meterable level instead of
> switching at 12 MHz. A healthy driving pin reads ~3.3 V. A healthy panel
> input reads about 8 kΩ to ground.

I added a `PIN_CHECK` mode that holds each half in each primary for ten
seconds — long enough to get probes onto a pin.

GPIO 4 read **0.6 V** driving, and the resistance check showed nothing at all —
`0.L`, open circuit. GPIO 5 next to it read 8 kΩ, exactly as expected. The pin
was damaged.

Fix: `8209a8b Move R1 from GPIO 4 to GPIO 8`. One line, after the measurement
made it certain.

<!-- PHOTO 04 -->
![Multimeter probing the R1 pin while the panel holds a solid red half](photos/04-metering.jpg)

> **Shoot:** multimeter leads on the header while the top half glows red (or
> stays dark, which is the actual fault). Get the reading on the display in
> focus. This is the money shot of the debugging section.

There is a coda: a second board on the bench had a damaged GPIO 5, and this one
had a damaged GPIO 4. Since the two projects needed different pins, the boards
were simply **swapped between projects** — each one avoiding its own dead pin.
Sometimes the fix is logistics.

---

## Part 4: video

<!-- VIDEO 01 -->
🎥 **`videos/01-playback.mp4`**

> **Shoot:** the panel playing a clip, filmed at **1/60 s or slower** and
> ideally 30 fps to reduce beating against the 488 Hz refresh. Handheld is
> fine; get close enough that the pixel grid is visible.

### Raw frames first

No filesystem, no codec: a dedicated `video` partition holding a 16-byte header
followed by RGB565 frames, and a player that reads one frame at a time into a
16 KiB buffer and calls `hub75_blit_rgb565()`.

The blit matters. Naively you would call `set_pixel` 8,192 times, each one
doing six read-modify-writes (one per bit plane). Instead it walks each (row,
plane) block once and writes both halves of a column in the same word, using
two small lookup tables indexed straight off the packed RGB565 fields — no
per-pixel unpacking arithmetic at all.

```c
static uint8_t s_gamma5[32];    /* RGB565 red/blue -> plane value */
static uint8_t s_gamma6[64];    /* RGB565 green    -> plane value */
```

Then the arithmetic bites:

| | |
|---|---|
| One frame | 16 KiB |
| One second at 15 fps | 240 KiB |
| A 5 MB partition | ~21 seconds |

Twenty-one seconds. That is a technical demo, not a thing you watch.

### MJPEG, and the chip that cannot help

The obvious answer is to compress. The S3 has no hardware JPEG decoder —
**that is the P4** (I said otherwise at one point and was wrong) — but it does
have **tjpgd in ROM**: a baseline JPEG decoder that costs zero flash and needs
about 3 KiB of scratch.

So the encoder gained `--mjpeg`: every frame is a JPEG, prefixed by a `u32`
size index so the player can seek without parsing.

The result is roughly **twenty times smaller** and still holds 30 fps on the
device. The clip currently on the panel is 408 frames of 128x64 at 30 fps in
**0.73 MiB** — under a megabyte for thirteen seconds that would have been
13 MB raw.

The player logs its **achieved** frame rate every 128 frames, not just its
target — a periodic heartbeat rather than a warning that only fires when
something is wrong. A late frame corrupts nothing, it just slows playback, so
without the heartbeat the difference between 30 fps and 22 fps is invisible.

### Bug 7 — the crop that cut through everyone's chest

I encoded a clip. The faces were gone; the panel showed a band of torsos.

The first report was *"you cut it horizontally, the long face is at the bottom,
the video itself was correct"* — which sounds like one problem and was two.

Instead of guessing, I extracted a single frame, **drew the candidate crop
band onto it**, and looked at the image:

1. The source was a 720x1280 portrait file, but the *content lay sideways
   inside it*. No rotation metadata on the stream — nothing in `ffprobe`
   suggests it. It needed `transpose=2`.
2. Faces in a portrait shot sit in the **upper third**. A centre crop lands on
   chests, every time.

```bash
./tools/encode_video.py clip.mp4 out.bin \
    --fps 30 --mjpeg --quality 9 --fit crop --rotate 270 --focus 0.2
```

`--focus` is new: 0 is the top edge, 1 the bottom, 0.5 the old centred
behaviour. It is evaluated against the *scaled* input, so it works whatever the
source aspect ratio.

> **The lesson:** one extracted frame with the crop drawn on it settled in
> sixty seconds what three rounds of guess-and-reflash had not. When the
> problem is visual, **look at it**. Do not reason about it.

<!-- PHOTO 05 -->
![Side by side: the centre crop landing on torsos, and focus 0.2 catching the faces](photos/05-crop.jpg)

> **Shoot:** two extracted frames with the crop rectangle drawn on, side by
> side. Regenerate these with ffmpeg + `drawbox` if you no longer have them.

---

## Part 5: making it useful

<!-- PHOTO 06 -->
![The four screens: clock, claude, now playing, video](photos/06-screens.jpg)

> **Shoot:** four photos, or one composite. The now-playing screen with
> artwork is the best-looking one — get that one right.

### Wi-Fi and the clock

Station mode with reconnect, SNTP, mDNS as `matrix-panel.local`, and
`TZ_STRING "NPT-5:45"` because Nepal is 5:45 ahead and there is no such thing
as a safe assumption about time zones.

The clock face, on 64 rows:

```
row  0   a green pixel sweeping left to right, one full pass per minute
row  5   DAY DD MON
row 19   HH:MM at 3x scale
row 42   whatever is playing, scrolling if it does not fit
row 52   Claude session quota, a bare 4-row bar
row 60   Claude weekly quota, a bare 4-row bar
```

No labels on the bars, no numbers. **The length is the reading.** A percentage
rendered at 5 pixels tall is unreadable from across a room; a bar is not.

The colon blinks once a second — the cheapest possible proof the display is
still being updated. And it blinks by being **drawn and then blanked**, not by
swapping in a space:

```c
gfx_text(hhmm_x, 19, hhmm, 3, 255, 170, 40);
if (tm.tm_sec & 1) {
    int colon_x = hhmm_x + 2 * gfx_char_advance('0', 3);
    gfx_fill_rect(colon_x, 19, 5 * 3, 7 * 3, 0, 0, 0);
}
```

Digits and the colon hold a fixed cell so the minutes never move. A space does
not hold a cell, so swapping one in would shove the digits sideways once a
second.

### Claude quota — and the route I refused

I wanted my Claude usage on the panel. The suggestion was to save browser
cookies from claude.ai and have the ESP32 replay them.

**I declined**, for two reasons worth stating: it is against the site's terms,
and it puts a live account credential on a LAN device that has an open HTTP
port and takes firmware over the network. That is a bad place for a session
token no matter how the code is written.

The legitimate route already existed. Claude Code stores an OAuth token in the
macOS keychain and there is a real usage endpoint,
`api.anthropic.com/api/oauth/usage`, which returns exactly the percentages
`/usage` shows. A small server on the Mac reads the token, calls that endpoint,
caches for 60 s, and serves plain JSON.

**The token never leaves the Mac. The panel only ever receives two integers.**

(I also initially claimed the percentage was not derivable from local data.
That was wrong — I was corrected with a working project that already did it.)

Getting the token itself had one wrinkle: it is **not** in
`~/.claude/.credentials.json` on macOS, it is in the keychain:

```bash
security find-generic-password -w -s "Claude Code-credentials"
```

Which is also why the server runs as a **LaunchAgent** and not a LaunchDaemon —
a daemon starts before login and has no keychain to read.

### Now playing — three dead ends and a working route

<!-- PHOTO 07 -->
![The now playing screen with album artwork](photos/07-nowplaying.jpg)

> **Shoot:** something with recognisable album art. The 48x48 thumbnail is the
> best demonstration in the project that this is a real display.

1. **MediaRemote** — the private framework everyone used for this. Apple
   gated it in macOS 15.4. Dead.
2. **AppleScript for the menu bar item** — needs the popover open to expose
   the labels, which means flashing UI on screen every poll. Unacceptable.
3. **The Media Session API** — browsers expose `navigator.mediaSession.metadata`
   to the page. Chromium browsers can be told to run JavaScript in a tab via
   AppleScript. That works, and it is the route.

Two failures on the way, both instructive:

- **AppleScript that failed to *compile*.** A `tell application "Google Chrome"`
  block fails at compile time if that app is not installed — not at run time,
  and not gracefully. Dynamic application names cannot resolve `active tab`
  either. The fix was to stop being clever and write one static block for the
  one browser actually in use.
- **A two-minute hang.** Iterating every tab in every window *wakes suspended
  tabs*. Restricting it to the active tab per window took it back to
  instantaneous.

Artwork is done on the Mac: ffmpeg scales the cover to 48x48 and converts to
RGB565, and the result is POSTed as exactly 4,608 raw bytes. The device does no
image decoding at all — it stores what it is given and never asks anyone for
anything.

That is a deliberate pattern across the whole project: **the panel is a
display, not a client.** It has no credentials, makes no outbound requests
except SNTP and the quota server on the LAN, and cannot leak anything it does
not have.

### The menu bar item, and a segfault I caused

A Hammerspoon module: click to toggle screens, a dropdown to pick one, icons
loaded as template images so macOS tints them for light and dark mode. It polls
`/status` every 30 s, so it tracks changes made from the web page or after a
reboot.

I crashed Hammerspoon during development — a hard segfault in `luaG_traceexec`.
My fault entirely: I had scheduled `hs.reload()` and then immediately hammered
the `hs` CLI in a tight loop, executing Lua inside a `lua_State` that was
tearing down.

**Never busy-poll a process you have just asked to restart itself.** Wait, then
query once.

(Also: after stripping the config down I dropped `require("hs.ipc")`, which is
what provides the `hs` CLI in the first place — so my diagnostic tool
disappeared at the exact moment I needed it. Restore it first.)

### Typography on a 5x7 font

The word spacing looked wrong — some gaps huge, others tight.

The font is drawn in fixed 5-column cells, but glyphs do not fill them. `I`
inks 3 columns; `:` inks 1. Advancing by the *cell* leaves a four-pixel hole
after a narrow letter while wide ones sit tight. That unevenness is what reads
as bad spacing.

Fix: advance by the **ink**, not the cell.

```c
static int char_advance(char c, int scale)
{
    if (fixed_width(c)) return (FONT5X7_W + GAP_COLS) * scale;
    /* ...otherwise measure the glyph's actual inked columns... */
    return (width + GAP_COLS) * scale;
}
```

With one exception: **digits and the colon keep the full cell.** A clock whose
glyphs changed width would shift sideways every time a 1 became a 2.

Proportional spacing everywhere else, monospace where alignment matters. That
is the same rule real typography uses for tabular figures, arrived at from the
opposite direction.

---

## Part 6: updates over Wi-Fi

The last piece, added the morning after. Reflashing over USB for a one-line
change is the thing that kills a project like this.

Two endpoints:

```bash
tools/push.sh fw                  # POST build/matrix_panel.bin -> /ota
tools/push.sh video demon.bin     # POST a clip -> /video, and play it
```

Both are also on the panel's own web page with a progress bar, which is the
easier route from a phone -- and both are in the menu bar dropdown, which is
the easier route from the machine that just built the firmware. **Upload
video...** even accepts a raw mp4 and runs it through ffmpeg first.

curl does the transfer rather than Lua, so a 3 MB image streams from disk, and
its progress meter is parsed back out of stderr to drive the menu bar title.
The pollers pause while it runs: the panel serves one request at a time, so a
status poll during an upload would just sit in the queue behind the upload.

<!-- PHOTO 08 -->
![The web page mid-upload, next to the panel showing its own progress bar](photos/08-ota.jpg)

> **Shoot:** phone in hand showing the upload progress bar, panel in the
> background showing `OTA 47%`. Both progress bars in one frame is the shot.

### The partition table had to change

OTA needs two application slots, and there was only a `factory`. The new
layout, on the 16 MB module:

```
nvs       0x9000     16K
otadata   0xd000      8K
phy_init  0xf000      4K
ota_0     0x10000   1.5M
ota_1     0x190000  1.5M
video     0x310000  12.94M
```

An app slot must start on a 64 KiB boundary, which is why the offsets are
written out rather than left blank — a stray size change silently shifts
`ota_1` and the bootloader then refuses the whole table.

The application is 976 KB, so 1.5 MB a slot leaves about 580 KB of growth —
and sizing the slots against the *binary* rather than a round number is what
freed the rest. The video partition went from 5M to **12.94M** in the same
edit, which is about four and a half minutes of MJPEG at 30 fps instead of
twenty-one seconds of raw frames.

That is the trade to make consciously, though: an image that outgrows its slot
is rejected by `/ota` outright and can only be delivered over USB. `idf.py
size` prints the number to watch.

Cost: **one** final USB flash to rewrite the table. Everything after that goes
over the network.

### Neither upload can brick it

This is where the care went, because an update mechanism that can leave the
device dead is worse than no update mechanism.

**Firmware** goes into whichever slot is *not* running. `esp_ota_end()`
verifies the image before it is made bootable, and the HTTP reply is sent
*before* the reboot fires 700 ms later. A dropped connection leaves the running
slot untouched. Rollback is enabled, and the app calls
`esp_ota_mark_app_valid_cancel_rollback()` only after its HTTP server is
answering — so "this image works" means *it joined Wi-Fi and served a request*,
not merely that it booted. Anything worse crash-loops and the bootloader puts
the old slot back.

**Video** holds the 16-byte header back and writes it **last**. An interrupted
upload therefore leaves an *unplayable* partition rather than one that plays
garbage. The header is validated — magic, 128x64, non-zero fps — *before*
anything is erased, so pushing the wrong file is a 400 with the old clip still
intact.

And the subtle one: the player holds the partition across an entire playback
loop, so the upload **stops it and waits for it** before erasing. Erasing flash
underneath a task that is reading it is the kind of bug that reproduces once a
fortnight and never on the bench.

```c
for (int i = 0; i < 60 && !video_idle(); i++) vTaskDelay(pdMS_TO_TICKS(50));
```

### It shows you what it is doing

During an upload the panel displays its own progress screen — label,
percentage, bar — and returns to whatever it was showing when it finishes. A
failure stays up for five seconds **with the reason on it**.

That screen deliberately sits outside the toggle cycle: `/toggle` skips it and
`/screen?s=update` will not select it. It is a state the device enters, not a
view you can ask for.

A push that dies halfway is otherwise completely invisible from across the
room, which is exactly when you are standing across the room.

---

## Part 7: how good can it actually look?

Once the update path existed, the interesting question stopped being "does it
work" and became "what is the ceiling". The honest answer required measuring
the panel against itself rather than squinting at it.

<!-- PHOTO 09 -->
![The same frame at two colour depths, side by side on the panel](photos/09-depth.jpg)

> **Shoot:** the same frame before and after the RGB888 change, same exposure,
> same brightness. The difference is in the dark regions and in smooth colour
> ramps — pick a frame with both.

### Measuring the display, not the file

I pulled eight uncompressed frames straight out of ffmpeg at 128x64, ran them
through the driver's *exact* lookup tables in Python, and compared the result
against what an infinitely precise panel would emit. No JPEG anywhere.

That gives a number nothing else can be judged without: **the panel's own
quantisation error is 0.667 RMS**, in units of its 0–63 plane values. That is
the floor. Any encoding error smaller than that is invisible by definition,
and any error much larger than it is the thing to go after.

The colour resolution behind that floor was the surprise:

```
red / blue   27 distinct levels     (not 32, and nowhere near 64)
green        46 distinct levels
             33,534 colours total
```

Six bit planes promise 262,144. The panel was delivering an eighth of that.

### Where the other seven eighths went

Two places. Gamma 2.2 crushes the bottom of the range — the first eight input
levels of green all map to plane value 0, so shadow detail simply is not
there. That one is inherent: it is the price of perceptually even steps out of
only 64 levels.

The other was **a conversion that should never have existed**. The video
player decoded each JPEG to RGB565 and then fed that to `hub75_blit_rgb565()`,
which expanded it back out through the gamma tables. But the ROM decoder's
output format is RGB888 — `JD_FORMAT 0`, right there in the header — so the
pipeline was:

```
tjpgd RGB888  ->  RGB565  ->  gamma  ->  6-bit planes
                  ^^^^^^ three bits of red and blue, discarded for nothing
```

The 565 step existed only because the blit function I wrote first happened to
take a `uint16_t *`. An API shape had quietly become a hardware limit.

Adding `hub75_blit_rgb888()` and letting `jpeg_out` copy rather than convert
takes it to **64 levels on every channel — 262,144 colours, a 7.8x
improvement** — and the output callback got *faster*, because a per-pixel
conversion became a `memcpy`. The cost is one byte per pixel in the frame
buffer: 24 KiB instead of 16.

> **The lesson:** I measured the panel's colour depth for the first time after
> it had been working for a day. The number was eight times worse than the
> design implied, and the cause was a line of my own glue code. Working is not
> the same as correct, and neither is the same as *measured*.

### Chroma, and two wrong answers

With the floor established, the encoder was worth measuring. Same eight frames,
compressed at a range of settings, decoded, pushed through the panel pipeline.

The measurement said 4:4:4 was a large win, so `--chroma 444` went into the
encoder. The panel went blank:

```
W (350697) video: frame 113 did not decode (tjpgd 8)
I (351164) video: frame 127/3150, 30.2 fps achieved, 0.7 ms/frame, 2% of budget
```

`tjpgd 8` is `JDR_FMT3`, "not supported JPEG standard" — every frame refused.
I had reasoned that 4:4:4 must be fine, being the simplest MCU layout, and
shipped that reasoning untested.

So I dumped the JPEG's own headers, found ffmpeg writing a luma sampling factor
of `0x12` where tjpgd wants `0x11`, `0x21` or `0x22`, and switched to 4:2:2,
whose luma factor is `0x22`. **The panel went blank again.** Same error.

The rule is not about luma alone. tjpgd requires the luma factor to be one of
those three *and both chroma factors to be exactly `0x11`* — and ffmpeg writes
neither 4:2:2 nor 4:4:4 that way:

```
ffmpeg yuvj420p   0x22 0x11 0x11   works
ffmpeg yuvj422p   0x22 0x12 0x12   rejected -- chroma is not 0x11
ffmpeg yuvj444p   0x12 0x12 0x12   rejected -- neither is
```

Both are legal JPEG, expressed with non-minimal sampling factors. The canonical
forms are `0x21 0x11 0x11` and `0x11 0x11 0x11`, and **libjpeg writes them**.
The whole diagnosis had been sitting in output I printed the first time round —
I had validated `f[0]` and not looked at the other two.

So the MJPEG path moved off ffmpeg's encoder onto **cjpeg**, with ffmpeg
reduced to producing PPM frames. All three chroma modes then decode.

### What the measurement actually says

With every mode available, compared at *matched bytes per frame* rather than
matched quality settings:

| | error | vs floor | bytes/frame |
|---|---|---|---|
| 4:4:4 q84 | 1.646 | 6.9× | 4204 |
| **4:2:2 q89** | **1.501** | **6.3×** | 4233 |
| 4:2:0 q95 | 1.900 | 8.0× | 5166 |

4:2:0 is clearly worst — it costs 22% more bytes than 4:2:2 and is 27% less
accurate. But **4:4:4 loses to 4:2:2 as well.** Full vertical chroma resolution
costs more bytes than it returns; spent on quantisation precision instead, the
same bytes buy more.

Which makes my earlier headline — "colour resolution beats quantisation" —
half right. Colour resolution beats quantisation *up to a point*, and 4:2:2 is
that point. The general rule I inferred from a two-column comparison did not
survive having a third column.

### The budget that decides it

None of that is free at 60 fps, and here the instrumentation paid for itself —
after I fixed it, because **the diagnostic was lying**.

The player logged a figure I had labelled "slack". It printed `next - now`
*before* advancing `next`, which means it was never slack at all: it was
approximately **negative the per-frame processing time**. Readings of
`-12749 us` looked like a panel falling twelve milliseconds behind. It was
actually a panel using 12.7 ms of a 16.67 ms budget and keeping perfect time.

A diagnostic that reads as a catastrophe when everything is fine is worse than
no diagnostic. It now prints what it means:

```
frame 767/9225, 60.0 fps achieved (60 target), 9.3 ms/frame, 56% of budget
```

With that, the trade is arithmetic rather than argument. At 60 fps and `-q 7`
in 4:2:0, the panel peaks at **91% of budget** — it holds 60.0 fps, but 4:4:4
costs more decode and there is nowhere for it to come from. At 30 fps the
budget doubles to 33 ms and all of it fits.

**30 fps at 4:2:2 beats 60 fps at 4:2:0** on this panel: better colour, far
less blocking, and — with no double buffer —
half as many chances to catch a blit mid-refresh. Higher frame rate was the
thing that looked like quality and was not.

---

## Part 8: skipping the encoder entirely

The panel now takes clips over wifi, but the loop was still encode, upload,
watch. The last step is to skip both: a TCP listener on 8089 that reads raw
RGB888 frames and blits them.

The arithmetic is friendlier than it looks. A frame is 24,576 bytes, so 30 fps
is 5.9 Mbps against the 15-30 Mbps an ESP32-S3 sustains. And it moves the cost
off the resource that was actually saturated: decoding a JPEG was ~14.7 ms of
a 16.7 ms budget at 60 fps, while the blit alone is a fraction of that. Trading
CPU for wifi is trading a resource there is none of for one there is plenty of.

The quality consequence is the interesting part. Every flashed clip has landed
6-7x the panel's own quantisation floor, because JPEG is the dominant error.
Streaming has no JPEG, so a streamed frame sits **at** the floor -- better than
anything that can be flashed, by a wide margin.

Each frame carries a four byte magic word, which is not about corruption: TCP
does not lose bytes. It catches a sender scaling to the wrong size, which would
otherwise shift the picture a little further on every frame and read as a
hardware fault. Testing that mattered more than testing the happy path, so both
were tested against a mock panel running the same parser: 537 frames with zero
resyncs on a correct sender, and 19 resyncs in 20 frames on one deliberately
scaling to 127x64.

`sendall` blocking when the panel falls behind is the whole flow control. No
queue, no dropped frames, no drift -- the sender simply runs at whatever rate
the panel can absorb.

---

## The numbers

| | |
|---|---|
| Panel | Q2.5AB32V4, 128x64 P2.5, 1/32 scan, HUB75E |
| MCU | ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM, unused) |
| Peripheral | LCD_CAM i80 bus, 16-bit, GDMA |
| Pixel clock | 12 MHz |
| Colour depth | 6 bit planes, 64 levels/channel, BCM |
| Displayable colours | 262,144 (was 33,534 before the RGB888 blit) |
| Panel error floor | 0.237 RMS of 63 levels (was 0.667) |
| Refresh buffer | 48 KiB internal DMA RAM, static |
| Refresh rate | ~488 Hz |
| CPU cost of refresh | one interrupt per frame |
| Video | MJPEG 4:2:2 via cjpeg, ROM tjpgd, 30 fps, 11.8 MiB for 105 s |
| Decode cost | ~9-15 ms/frame; 60 fps fits, with 4:2:2 |
| Streaming | raw RGB888 over TCP, 5.9 Mbps at 30 fps, no compression error |
| Firmware update | HTTP POST, dual-slot, verified, auto-rollback |
| Commits | 48 |
| Dead GPIOs discovered | 2 (one per board, different pins) |

---

## What I would tell someone starting this

**Write the driver.** It is about 400 lines. You will be debugging signal
timing whether or not you understand it, and the library will not help you
read a multimeter.

**A word is a clock is a pixel.** Almost every geometric artefact — shifts,
ghosts, wrong rows — comes from the word count of a block not matching the
panel's expectations. Count words before suspecting timing.

**Exonerate the software before accusing the hardware.** I got this backwards
once, on the gamma bug, and sent someone to the bench for nothing. The
symptom's *shape* usually says which side it is on: a fault that depends on the
pixel value is arithmetic, and a fault that depends on physical position is a
wire.

**But do use the multimeter when the shape says hardware.** Holding one half of
the panel in one solid colour turns a 12 MHz signal into a DC level anyone can
measure, and it found a genuinely dead GPIO in about ninety seconds.

**When the problem is visual, look at it.** Extracting one frame with the crop
box drawn on it beat three rounds of re-encode-and-reflash.

**Know when the bug is not yours.** Six commits chasing a ghost row that turned
out to be the panel. The tell was that it moved with the content and ignored
every timing change.

**Measure the thing itself, not just its output.** The panel had been working
for a day before I put eight frames through its own lookup tables and found it
was showing an eighth of the colours it should. Nothing looked broken. The bug
was in glue code I had written on the first afternoon.

**Check every field, not the one your theory is about.** I diagnosed a decoder
rejection twice, wrongly, from output that contained the answer both times. I
was looking at the luma sampling factor because that was what my theory
concerned, and the chroma factors were printed right beside it.

**A wrong label on a diagnostic is worse than no diagnostic.** "-12749 us
slack" reads as failure. It meant 56% of budget used. I wrote both the number
and the label, and still misread it a month later.

**Keep the credentials off the device.** The panel has an open HTTP port and
accepts firmware over the network. It gets percentages and pixels, never
tokens.

**Build the update path early.** Everything after `tools/push.sh` existed got
faster, and the last few features would probably not have been built at all
behind a USB cable.

---

## Photographing an LED matrix

Genuinely non-obvious, and it will ruin your photos if you get it wrong.

- The panel refreshes at ~488 Hz and each bit plane is lit for a different
  duration. A fast shutter catches **one bit plane** and the colours come out
  wrong — usually too dark and oddly hued.
- Shoot at **1/60 s or slower**. Slower is better. Use a tripod and lower the
  ISO rather than raising the shutter speed.
- For video, **30 fps with a 1/60 s shutter** minimises the rolling beat
  pattern. It will not vanish entirely; that is the refresh, not your camera.
- Turn the room lights down and the panel brightness down. `hub75_set_brightness(128)`
  photographs far better than 255, which clips to white and blooms.
- Shoot slightly off-axis. Straight on, the LEDs are point sources and the
  image reads as glare; at an angle the pixel grid reads as a grid.

---

## Repo

`github.com/sande5h/matrix_panel` — ESP-IDF v6, ESP32-S3.

```
components/hub75/     the driver: i80 bus, BCM planes, DMA refresh
main/                 screens, HTTP API, Wi-Fi, OTA
tools/encode_video.py ffmpeg -> RGB565/MJPEG blob
tools/push.sh         push firmware or a clip over Wi-Fi
tools/matrix_panel.lua  Hammerspoon menu bar item
```

Wi-Fi credentials live in `main/secrets.h`, which is gitignored;
`main/secrets.example.h` is the template.

<!-- VIDEO 02 -->
🎥 **`videos/02-tour.mp4`**

> **Shoot:** a 30-second tour — toggle through all four screens from the menu
> bar item, then push a clip over Wi-Fi and let it start playing. That single
> clip demonstrates the whole project.
