#pragma once

/* Bring-up helpers, compiled in but only called when main.c enables them.

   pin_check() holds each half of the panel in each primary for 5 s. A
   channel's data bit is then set in every word of every block, so the pin
   sits at a steady level a multimeter reads directly: ~3.3 V if the S3 is
   driving it, ~0 V if it is not. That is how a dead GPIO was told apart from
   a loose wire during bring-up.

   self_test() is the quicker confidence check: each half in each primary,
   then a one pixel border whose edges must land on the outermost rows and
   columns.

   plasma() is the fallback animation when there is nothing else to show. */
void pin_check(void);
void self_test(void);
void plasma(float t);
