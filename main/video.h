#pragma once

#include <stdbool.h>

/* Plays the clip in the 'video' partition, blitting frames to the panel at the
 * rate it was encoded at. Returns false if no valid clip is flashed, so the
 * caller can fall back to something else. */
bool video_play(bool loop);
