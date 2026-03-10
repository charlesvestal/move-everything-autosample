#include "loop_finder.h"
#include "third_party/kissfft/kiss_fft.h"
#include "third_party/kissfft/kiss_fftr.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FFT_SIZE        2048
#define HOP_SIZE        1024
#define MAG_BINS        512
#define MIN_WINDOWS     4
#define MIN_WINDOW_SEP  5       /* ~100ms at 1024-hop / 44100 */
#define MAX_CANDIDATES  20
#define ZC_SEARCH       132     /* ±3ms at 44100 */
#define CORR_LEN        441     /* 10ms at 44100 */
#define MIN_CORRELATION 0.9f

typedef struct {
    int win_a;
    int win_b;
    float distance;
} candidate_t;

typedef struct {
    int start;
    int end;
    float correlation;
} refined_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void compute_magnitude(kiss_fftr_cfg cfg, const int16_t *mono,
                              int offset, float *mag)
{
    kiss_fft_scalar in[FFT_SIZE];
    kiss_fft_cpx out[FFT_SIZE / 2 + 1];

    for (int i = 0; i < FFT_SIZE; i++)
        in[i] = (kiss_fft_scalar)mono[offset + i];

    kiss_fftr(cfg, in, out);

    for (int k = 0; k < MAG_BINS; k++) {
        float r = (float)out[k].r;
        float im = (float)out[k].i;
        mag[k] = logf(1.0f + sqrtf(r * r + im * im));
    }
}

static float spectral_distance(const float *a, const float *b)
{
    float sum = 0.0f;
    for (int k = 0; k < MAG_BINS; k++) {
        float d = a[k] - b[k];
        sum += d * d;
    }
    return sum;
}

static void insert_candidate(candidate_t *cands, int *count,
                              int win_a, int win_b, float dist)
{
    /* Find insertion position (sorted ascending by distance). */
    int pos = *count;
    for (int i = 0; i < *count; i++) {
        if (dist < cands[i].distance) {
            pos = i;
            break;
        }
    }

    if (pos >= MAX_CANDIDATES)
        return;

    /* Shift elements right. */
    int end = (*count < MAX_CANDIDATES) ? *count : MAX_CANDIDATES - 1;
    for (int i = end; i > pos; i--)
        cands[i] = cands[i - 1];

    cands[pos].win_a = win_a;
    cands[pos].win_b = win_b;
    cands[pos].distance = dist;

    if (*count < MAX_CANDIDATES)
        (*count)++;
}

static int is_zero_crossing(const int16_t *mono, int i)
{
    return (mono[i] >= 0 && mono[i + 1] < 0) ||
           (mono[i] < 0 && mono[i + 1] >= 0);
}

static float normalized_cross_correlation(const int16_t *mono,
                                          int start, int end, int len)
{
    double sum_ab = 0.0, sum_aa = 0.0, sum_bb = 0.0;
    for (int i = 0; i < len; i++) {
        double a = (double)mono[start + i];
        double b = (double)mono[end + i];
        sum_ab += a * b;
        sum_aa += a * a;
        sum_bb += b * b;
    }
    double denom = sqrt(sum_aa * sum_bb);
    if (denom < 1e-12)
        return 0.0f;
    return (float)(sum_ab / denom);
}

/* ------------------------------------------------------------------ */
/* Main entry point                                                    */
/* ------------------------------------------------------------------ */

loop_result_t loop_find(const int16_t *stereo_samples,
                        int num_frames,
                        int skip_frames,
                        int sustain_end_frame)
{
    loop_result_t result;
    memset(&result, 0, sizeof(result));

    if (sustain_end_frame > num_frames)
        sustain_end_frame = num_frames;

    int sustain_length = sustain_end_frame - skip_frames;
    if (sustain_length < FFT_SIZE)
        return result;

    int num_windows = (sustain_length - FFT_SIZE) / HOP_SIZE + 1;
    if (num_windows < MIN_WINDOWS)
        return result;

    /* --- Mono mixdown of sustain region --- */
    int16_t *mono = (int16_t *)malloc(sustain_length * sizeof(int16_t));
    if (!mono)
        return result;

    for (int i = 0; i < sustain_length; i++) {
        int idx = (skip_frames + i) * 2;
        mono[i] = (int16_t)(((int)stereo_samples[idx] +
                              (int)stereo_samples[idx + 1]) / 2);
    }

    /* --- Compute magnitude spectra for all windows --- */
    float *mags = (float *)malloc(num_windows * MAG_BINS * sizeof(float));
    kiss_fftr_cfg cfg = kiss_fftr_alloc(FFT_SIZE, 0, NULL, NULL);
    if (!mags || !cfg) {
        free(mono);
        free(mags);
        free(cfg);
        return result;
    }

    for (int w = 0; w < num_windows; w++)
        compute_magnitude(cfg, mono, w * HOP_SIZE, &mags[w * MAG_BINS]);

    kiss_fft_free(cfg);

    /* --- Find top candidates (most similar non-adjacent pairs) --- */
    candidate_t cands[MAX_CANDIDATES];
    int cand_count = 0;

    for (int a = 0; a < num_windows; a++) {
        for (int b = a + MIN_WINDOW_SEP; b < num_windows; b++) {
            float dist = spectral_distance(&mags[a * MAG_BINS],
                                           &mags[b * MAG_BINS]);
            /* Only insert if better than worst or not full yet. */
            if (cand_count < MAX_CANDIDATES ||
                dist < cands[cand_count - 1].distance) {
                insert_candidate(cands, &cand_count, a, b, dist);
            }
        }
    }

    free(mags);

    /* --- Waveform refinement --- */
    refined_t best;
    best.correlation = -1.0f;
    best.start = 0;
    best.end = 0;

    for (int c = 0; c < cand_count; c++) {
        int start_pos = cands[c].win_a * HOP_SIZE + FFT_SIZE / 2;
        int end_pos   = cands[c].win_b * HOP_SIZE + FFT_SIZE / 2;

        /* Search for zero crossings near start_pos. */
        for (int ds = -ZC_SEARCH; ds <= ZC_SEARCH; ds++) {
            int s = start_pos + ds;
            if (s < 0 || s + CORR_LEN >= sustain_length)
                continue;
            if (!is_zero_crossing(mono, s))
                continue;

            /* Search for zero crossings near end_pos. */
            for (int de = -ZC_SEARCH; de <= ZC_SEARCH; de++) {
                int e = end_pos + de;
                if (e < 0 || e + CORR_LEN >= sustain_length)
                    continue;
                if (e <= s)
                    continue;
                if (!is_zero_crossing(mono, e))
                    continue;

                float corr = normalized_cross_correlation(mono, s, e,
                                                          CORR_LEN);
                if (corr > best.correlation) {
                    best.correlation = corr;
                    best.start = s;
                    best.end = e;
                }
            }
        }
    }

    free(mono);

    if (best.correlation < MIN_CORRELATION)
        return result;

    /* --- Score with length bonus --- */
    int loop_length = best.end - best.start;
    float length_bonus = (float)loop_length / 22050.0f;
    if (length_bonus > 1.0f)
        length_bonus = 1.0f;
    float score = best.correlation * length_bonus;
    (void)score; /* single candidate set, score used implicitly */

    /* --- Compute crossfade --- */
    int crossfade = loop_length / 10;
    if (crossfade < 220)
        crossfade = 220;
    if (crossfade > 2205)
        crossfade = 2205;

    /* --- Build result (translate back to original stereo frame indices) --- */
    result.found = 1;
    result.loop_start = best.start + skip_frames;
    result.loop_end = best.end + skip_frames;
    result.loop_crossfade = crossfade;
    result.quality = best.correlation;

    return result;
}
