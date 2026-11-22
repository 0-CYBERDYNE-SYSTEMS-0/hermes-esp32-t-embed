/* How much of a PCM16 write fits in the speaker's FreeRTOS stream buffer.
 *
 * A stream buffer always keeps one byte free, so its room is odd whenever it holds whole
 * samples. Queueing all of that room would split a 16-bit sample, shift every later sample by
 * one byte and play the rest of the reply as noise. Only whole samples are queued.
 *
 * Same fix as the hermes-gadget-sdk reference firmware (firmware/drivers/speaker_queue.hpp,
 * MIT). Header-only and free of FreeRTOS so it is unit-tested on the host.
 */
#pragma once

#include <stddef.h>

static inline size_t t_embed_whole_sample_bytes(size_t want, size_t room) {
    size_t n = want < room ? want : room;
    return n & ~(size_t)1;
}
