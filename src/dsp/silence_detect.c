#include "silence_detect.h"
#include <math.h>

float silence_rms(const int16_t *stereo, int num_frames) {
    if (num_frames <= 0) return 0.0f;

    double sum_sq = 0.0;
    for (int i = 0; i < num_frames; i++) {
        float mono = (float)(stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
        sum_sq += (double)(mono * mono);
    }
    return sqrtf((float)(sum_sq / num_frames));
}

int silence_find_onset(const int16_t *stereo, int num_frames,
                       float threshold_rms, int window_frames,
                       int safety_frames) {
    if (num_frames < window_frames || window_frames <= 0) return -1;

    int limit = num_frames - window_frames;
    for (int pos = 0; pos <= limit; pos++) {
        float rms = silence_rms(stereo + pos * 2, window_frames);
        if (rms > threshold_rms) {
            int onset = pos - safety_frames;
            return onset > 0 ? onset : 0;
        }
    }
    return -1;
}

int silence_find_tail(const int16_t *stereo, int num_frames,
                      float threshold_rms, int window_frames,
                      int duration_frames) {
    if (num_frames < window_frames || window_frames <= 0) return num_frames;

    int last_above = -1;
    int limit = num_frames - window_frames;
    for (int pos = 0; pos <= limit; pos++) {
        float rms = silence_rms(stereo + pos * 2, window_frames);
        if (rms > threshold_rms) {
            last_above = pos;
        }
    }

    if (last_above < 0) {
        /* Signal never exceeded threshold — entire buffer is silence */
        return 0;
    }

    int tail_start = last_above + window_frames + duration_frames;
    if (tail_start > num_frames) {
        return num_frames;
    }
    return tail_start;
}
