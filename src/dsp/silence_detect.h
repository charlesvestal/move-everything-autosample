#ifndef SILENCE_DETECT_H
#define SILENCE_DETECT_H

#include <stdint.h>

/* Compute RMS of stereo interleaved int16 buffer (mono mixdown).
 * Returns RMS as float (0.0 to 32768.0 range). */
float silence_rms(const int16_t *stereo, int num_frames);

/* Find first frame where signal exceeds threshold.
 * Uses sliding window RMS. Returns frame index, or -1 if not found.
 * Backs up safety_frames before the detected onset. */
int silence_find_onset(const int16_t *stereo, int num_frames,
                       float threshold_rms, int window_frames,
                       int safety_frames);

/* Find frame where signal drops below threshold for duration_frames.
 * Searches forward from the end of the sustain. Returns the frame
 * where silence begins, or num_frames if no silence found. */
int silence_find_tail(const int16_t *stereo, int num_frames,
                      float threshold_rms, int window_frames,
                      int duration_frames);

#endif
