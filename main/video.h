#pragma once

#include <stdbool.h>

/* Plays the clip in the 'video' partition, blitting frames to the panel at the
 * rate it was encoded at. Returns false if no valid clip is flashed, so the
 * caller can fall back to something else.
 *
 * keep_going is checked once per frame and playback stops as soon as it
 * returns false, which is how a screen change interrupts a clip without
 * waiting for it to finish. Pass NULL to play to the end.
 */
bool video_play(bool loop, bool (*keep_going)(void));
