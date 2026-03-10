#ifndef SFZ_WRITER_H
#define SFZ_WRITER_H

#include <stdint.h>

#define SFZ_MAX_ZONES 24
#define SFZ_MAX_LAYERS 8

typedef struct {
    int midi_note;          /* MIDI note that was sampled */
    int velocity;           /* velocity that was sampled */
    char filename[128];     /* relative path: samples/C2_v042.wav */
    int has_loop;           /* 0 or 1 */
    int loop_start;         /* sample frame */
    int loop_end;           /* sample frame */
    int loop_crossfade;     /* sample count for crossfade */
} sfz_sample_info_t;

typedef struct {
    char instrument_name[128];
    int num_zones;
    int num_layers;
    int range_low;          /* lowest MIDI note in range */
    int range_high;         /* highest MIDI note in range */
    int sample_notes[SFZ_MAX_ZONES];        /* MIDI notes sampled */
    int sample_velocities[SFZ_MAX_LAYERS];  /* velocities sampled */
    sfz_sample_info_t samples[SFZ_MAX_ZONES * SFZ_MAX_LAYERS];
    int sample_count;
} sfz_instrument_t;

/* Write .sfz file. Returns 0 on success, -1 on error. */
int sfz_write(const char *path, const sfz_instrument_t *inst);

#endif
