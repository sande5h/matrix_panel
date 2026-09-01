#pragma once

#include <stdbool.h>
#include <stdint.h>

/* What the encoder wrote into the first 16 bytes of the video partition. The
 * header is little endian and the same layout tools/encode_video.py emits. */
typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t frames;
} __attribute__((packed)) video_header_t;

#define VIDEO_MAGIC_RAW    0x35374248u   /* "HB75" -- RGB565 frames, no index   */
#define VIDEO_MAGIC_MJPEG  0x4A374248u   /* "HB7J" -- JPEG frames, u32 size each */
#define VIDEO_HEADER_BYTES 16

/* Plays the clip in the 'video' partition, blitting frames to the panel at the
 * rate it was encoded at. Returns false if no valid clip is flashed, so the
 * caller can fall back to something else.
 *
 * keep_going is checked once per frame and playback stops as soon as it
 * returns false, which is how a screen change interrupts a clip without
 * waiting for it to finish. Pass NULL to play to the end.
 */
bool video_play(bool loop, bool (*keep_going)(void));

/* False while video_play is inside the partition. An upload has to wait for
 * this before erasing, or the player reads frames out of half-written flash. */
bool video_idle(void);

/* The flashed clip's header, or false if the partition holds no valid one. */
bool video_info(video_header_t *out);
