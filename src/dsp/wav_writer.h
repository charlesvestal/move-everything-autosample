#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <stdint.h>

/* Write a stereo 16-bit 44100Hz WAV file.
 * If loop_start >= 0 and loop_end > loop_start, includes a smpl chunk.
 * Returns 0 on success, -1 on error. */
int wav_write(const char *path,
              const int16_t *interleaved_stereo,
              int num_frames,
              int loop_start,   /* -1 for no loop */
              int loop_end);    /* sample frame (not byte offset) */

#endif
