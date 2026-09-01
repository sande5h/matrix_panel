#pragma once

#include <stdbool.h>

/* Starts the frame listener. The Mac connects and pushes RGB888 frames as
 * fast as it likes; the panel shows them and forgets them.
 *
 * This is the shortest path from a picture on the Mac to a picture on the
 * panel: no encoder, no partition, no flashing, and because nothing is
 * compressed there is no compression error either -- a streamed frame is as
 * close to the source as the panel's own six bit planes can get, which is
 * several times better than the best flashed clip. */
void stream_start(void);

/* True while a sender is connected. */
bool stream_active(void);
