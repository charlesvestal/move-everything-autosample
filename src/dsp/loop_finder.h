#ifndef LOOP_FINDER_H
#define LOOP_FINDER_H

#include <stdint.h>

typedef struct {
    int found;              /* 1 if a good loop was found, 0 otherwise */
    int loop_start;         /* sample frame */
    int loop_end;           /* sample frame */
    int loop_crossfade;     /* recommended crossfade in samples */
    float quality;          /* 0.0-1.0, correlation quality */
} loop_result_t;

/* Find optimal loop points in a stereo sample.
 * Analyzes mono mixdown of the sustain region (skip_frames..sustain_end_frame).
 * Returns result with found=0 if no acceptable loop found. */
loop_result_t loop_find(const int16_t *stereo_samples,
                        int num_frames,
                        int skip_frames,        /* frames to skip (attack) */
                        int sustain_end_frame);  /* end of sustain region */

#endif
