/*
 * Sample Robot DSP Plugin
 *
 * Autosamples external MIDI gear via USB MIDI out + line-in audio.
 * Produces SFZ instruments compatible with the SFZ Player module.
 *
 * V2 API - instance-based.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

#include "wav_writer.h"
#include "silence_detect.h"
#include "sfz_writer.h"
#include "loop_finder.h"

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

typedef void (*move_mod_emit_value_fn)(void *ctx, int source_id, float value);
typedef void (*move_mod_clear_source_fn)(void *ctx, int source_id);

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
} host_api_v1_t;

typedef struct plugin_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    int (*get_error)(void *instance, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_interleaved_lr, int frames);
} plugin_api_v2_t;

static const host_api_v1_t *g_host = NULL;

/* ---- State machine ---- */

typedef enum {
    STATE_IDLE = 0,
    STATE_PRE_SAMPLE,
    STATE_RECORDING,
    STATE_RELEASE,
    STATE_PROCESSING,
    STATE_DONE
} sample_state_t;

#define MAX_CAPTURE_SECONDS 35  /* hold + release + margin */
#define MAX_CAPTURE_FRAMES (MAX_CAPTURE_SECONDS * MOVE_SAMPLE_RATE)

/* Note name tables */
static const char *NOTE_NAMES[] = {"C","Cs","D","Ds","E","F","Fs","G","Gs","A","As","B"};
static const char *NOTE_DISPLAY[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};

typedef struct {
    /* Config */
    int range_low;
    int range_high;
    int key_zones;
    int velocity_layers;
    float hold_duration;
    int loop_detect;
    int midi_channel;       /* 1-16 */
    char instrument_name[128];
    char module_dir[512];

    /* State */
    sample_state_t state;
    int current_zone;
    int current_layer;
    int total_samples;
    int completed_samples;
    int skipped_samples;
    int loops_found;

    /* Audio capture */
    int16_t *capture_buf;
    int capture_frames;

    /* Noise floor */
    float noise_floor_rms;

    /* Timing */
    int block_counter;
    int hold_blocks;

    /* Schedule */
    int sample_notes[SFZ_MAX_ZONES];
    int sample_velocities[SFZ_MAX_LAYERS];

    /* SFZ instrument being built */
    sfz_instrument_t instrument;

    /* Output paths */
    char output_dir[512];   /* .../instruments/<name>/ */
    char samples_dir[512];  /* .../instruments/<name>/samples/ */

    /* Status for UI polling */
    char status_text[128];

    /* Pending MIDI for JS to send via move_midi_external_send */
    int midi_pending;       /* 0=none, 1=note_on, 2=note_off */
    int midi_note;
    int midi_vel;
    int midi_ch;            /* 0-15 */
} samplerobot_instance_t;

/* ---- Helper: mkdir_p ---- */

static void mkdir_p(const char *path) {
    char tmp[512];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

/* ---- Schedule ---- */

static void compute_schedule(samplerobot_instance_t *inst) {
    /* Evenly space notes */
    for (int i = 0; i < inst->key_zones; i++) {
        if (inst->key_zones == 1)
            inst->sample_notes[i] = (inst->range_low + inst->range_high) / 2;
        else
            inst->sample_notes[i] = inst->range_low +
                (i * (inst->range_high - inst->range_low) + (inst->key_zones - 1) / 2) / (inst->key_zones - 1);
    }
    /* Evenly space velocities */
    for (int i = 0; i < inst->velocity_layers; i++) {
        if (inst->velocity_layers == 1)
            inst->sample_velocities[i] = 100;
        else
            inst->sample_velocities[i] = 1 + (i * 126) / (inst->velocity_layers - 1);
    }
    inst->total_samples = inst->key_zones * inst->velocity_layers;
    inst->completed_samples = 0;
    inst->skipped_samples = 0;
    inst->loops_found = 0;
    inst->current_zone = 0;
    inst->current_layer = 0;
}

/* ---- MIDI helpers ---- */

static void send_note_on(samplerobot_instance_t *inst) {
    int note = inst->sample_notes[inst->current_zone];
    int vel = inst->sample_velocities[inst->current_layer];
    inst->midi_pending = 1;  /* note on */
    inst->midi_note = note;
    inst->midi_vel = vel;
    inst->midi_ch = inst->midi_channel - 1;  /* 0-based */
    if (g_host && g_host->log) {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "SampleRobot: Note ON pending ch=%d note=%d vel=%d",
                 inst->midi_channel, note, vel);
        g_host->log(log_msg);
    }
}

static void send_note_off(samplerobot_instance_t *inst) {
    int note = inst->sample_notes[inst->current_zone];
    inst->midi_pending = 2;  /* note off */
    inst->midi_note = note;
    inst->midi_vel = 0;
    inst->midi_ch = inst->midi_channel - 1;
    if (g_host && g_host->log) {
        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "SampleRobot: Note OFF pending ch=%d note=%d",
                 inst->midi_channel, note);
        g_host->log(log_msg);
    }
}

/* ---- Audio capture ---- */

static void append_audio(samplerobot_instance_t *inst, const int16_t *audio_in, int frames) {
    int remaining = MAX_CAPTURE_FRAMES - inst->capture_frames;
    if (remaining <= 0) return;
    int to_copy = frames < remaining ? frames : remaining;
    memcpy(inst->capture_buf + inst->capture_frames * 2,
           audio_in,
           to_copy * 2 * sizeof(int16_t));
    inst->capture_frames += to_copy;
}

/* ---- Forward declarations ---- */

static void process_and_advance(samplerobot_instance_t *inst);
static void finish_instrument(samplerobot_instance_t *inst);

/* ---- Start sampling ---- */

static void start_sampling(samplerobot_instance_t *inst) {
    /* Build output paths */
    snprintf(inst->output_dir, sizeof(inst->output_dir),
             "/data/UserData/move-anything/modules/sound_generators/sfz/instruments/%s/",
             inst->instrument_name);
    snprintf(inst->samples_dir, sizeof(inst->samples_dir),
             "%ssamples/", inst->output_dir);

    /* Create directories */
    mkdir_p(inst->output_dir);
    mkdir_p(inst->samples_dir);

    /* Initialize instrument struct */
    memset(&inst->instrument, 0, sizeof(sfz_instrument_t));
    strncpy(inst->instrument.instrument_name, inst->instrument_name,
            sizeof(inst->instrument.instrument_name) - 1);
    inst->instrument.num_zones = inst->key_zones;
    inst->instrument.num_layers = inst->velocity_layers;
    inst->instrument.range_low = inst->range_low;
    inst->instrument.range_high = inst->range_high;
    memcpy(inst->instrument.sample_notes, inst->sample_notes,
           sizeof(int) * inst->key_zones);
    memcpy(inst->instrument.sample_velocities, inst->sample_velocities,
           sizeof(int) * inst->velocity_layers);
    inst->instrument.sample_count = 0;

    /* Initialize state */
    inst->state = STATE_PRE_SAMPLE;
    inst->block_counter = 0;
    inst->capture_frames = 0;
    inst->hold_blocks = (int)(inst->hold_duration * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);

    snprintf(inst->status_text, sizeof(inst->status_text), "Measuring noise floor...");
}

/* ---- Process and advance ---- */

static void process_and_advance(samplerobot_instance_t *inst) {
    int note = inst->sample_notes[inst->current_zone];
    int vel = inst->sample_velocities[inst->current_layer];
    int octave = (note / 12) - 1;
    int note_idx = note % 12;

    /* Log peak and RMS of entire capture for diagnostics */
    if (g_host && g_host->log) {
        int16_t peak = 0;
        for (int i = 0; i < inst->capture_frames * 2; i++) {
            int16_t s = inst->capture_buf[i] < 0 ? -inst->capture_buf[i] : inst->capture_buf[i];
            if (s > peak) peak = s;
        }
        float full_rms = silence_rms(inst->capture_buf, inst->capture_frames);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "SampleRobot: process %s%d v%d - capture_frames=%d peak=%d full_rms=%.2f noise_rms=%.2f",
                 NOTE_DISPLAY[note_idx], octave, vel,
                 inst->capture_frames, peak, full_rms, inst->noise_floor_rms);
        g_host->log(msg);
    }

    /* Trim leading silence */
    float onset_thresh = inst->noise_floor_rms * 4.0f;  /* ~12dB above */
    int onset = silence_find_onset(inst->capture_buf, inst->capture_frames,
                                   onset_thresh, 4410, 88);

    /* Trim trailing silence */
    float tail_thresh = inst->noise_floor_rms * 2.0f;   /* ~6dB above */
    int tail = silence_find_tail(inst->capture_buf, inst->capture_frames,
                                 tail_thresh, 8820, 8820);

    /* Validate */
    if (onset < 0 || tail - onset < 4410) {
        /* Skip this sample */
        inst->skipped_samples++;
        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "Skipped %s%d v%d (onset=%d tail=%d frames=%d noise_rms=%.2f thresh=%.2f)",
                 NOTE_DISPLAY[note_idx], octave, vel,
                 onset, tail, inst->capture_frames,
                 inst->noise_floor_rms, onset_thresh);
        if (g_host->log) g_host->log(log_msg);
    } else {
        /* Trimmed buffer */
        int16_t *trimmed = inst->capture_buf + onset * 2;
        int trimmed_len = tail - onset;

        /* Loop detection */
        loop_result_t loop_res = {0};
        if (inst->loop_detect) {
            int sustain_end = inst->hold_blocks * MOVE_FRAMES_PER_BLOCK - onset;
            if (sustain_end > trimmed_len) sustain_end = trimmed_len;
            int skip_attack = 8820;  /* 200ms */
            if (skip_attack < sustain_end) {
                loop_res = loop_find(trimmed, trimmed_len, skip_attack, sustain_end);
            }
        }

        /* Build filename */
        char filename[128];
        snprintf(filename, sizeof(filename), "samples/%s%d_v%03d.wav",
                 NOTE_NAMES[note_idx], octave, vel);

        /* Build full path */
        char full_path[640];
        snprintf(full_path, sizeof(full_path), "%s%s", inst->output_dir, filename);

        /* Write WAV */
        int loop_start = loop_res.found ? loop_res.loop_start : -1;
        int loop_end = loop_res.found ? loop_res.loop_end : -1;
        wav_write(full_path, trimmed, trimmed_len, loop_start, loop_end);

        /* Populate SFZ sample info */
        sfz_sample_info_t *info = &inst->instrument.samples[inst->instrument.sample_count];
        info->midi_note = note;
        info->velocity = vel;
        strncpy(info->filename, filename, sizeof(info->filename) - 1);
        info->has_loop = loop_res.found;
        info->loop_start = loop_res.found ? loop_res.loop_start : 0;
        info->loop_end = loop_res.found ? loop_res.loop_end : 0;
        info->loop_crossfade = loop_res.found ? loop_res.loop_crossfade : 0;
        inst->instrument.sample_count++;

        inst->completed_samples++;
        if (loop_res.found) inst->loops_found++;

        char log_msg[256];
        snprintf(log_msg, sizeof(log_msg),
                 "Captured %s%d v%d (%d frames%s)",
                 NOTE_DISPLAY[note_idx], octave, vel, trimmed_len,
                 loop_res.found ? ", loop found" : "");
        if (g_host->log) g_host->log(log_msg);
    }

    /* Advance to next sample */
    inst->current_layer++;
    if (inst->current_layer >= inst->velocity_layers) {
        inst->current_layer = 0;
        inst->current_zone++;
    }

    if (inst->current_zone >= inst->key_zones) {
        /* All done */
        finish_instrument(inst);
        inst->state = STATE_DONE;
    } else {
        /* Next sample */
        inst->state = STATE_PRE_SAMPLE;
        inst->block_counter = 0;
        inst->capture_frames = 0;

        int next_note = inst->sample_notes[inst->current_zone];
        int next_vel = inst->sample_velocities[inst->current_layer];
        int next_idx = next_note % 12;
        int next_oct = (next_note / 12) - 1;
        snprintf(inst->status_text, sizeof(inst->status_text),
                 "Next: %s%d v%d (%d/%d)",
                 NOTE_DISPLAY[next_idx], next_oct, next_vel,
                 inst->completed_samples + inst->skipped_samples + 1,
                 inst->total_samples);
    }
}

/* ---- Finish instrument ---- */

static void finish_instrument(samplerobot_instance_t *inst) {
    /* Write SFZ file */
    char sfz_path[640];
    snprintf(sfz_path, sizeof(sfz_path), "%s%s.sfz",
             inst->output_dir, inst->instrument_name);

    sfz_write(sfz_path, &inst->instrument);

    snprintf(inst->status_text, sizeof(inst->status_text),
             "Done! %d samples, %d loops, %d skipped",
             inst->completed_samples, inst->loops_found, inst->skipped_samples);

    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg),
             "Sample Robot: finished '%s' - %d samples, %d loops, %d skipped",
             inst->instrument_name, inst->completed_samples,
             inst->loops_found, inst->skipped_samples);
    if (g_host->log) g_host->log(log_msg);
}

/* ---- Plugin API functions ---- */

static void* sr_create_instance(const char *module_dir, const char *json_defaults) {
    samplerobot_instance_t *inst = calloc(1, sizeof(samplerobot_instance_t));
    if (!inst) return NULL;

    /* Defaults */
    inst->range_low = 36;
    inst->range_high = 84;
    inst->key_zones = 2;
    inst->velocity_layers = 2;
    inst->hold_duration = 3.0f;
    inst->loop_detect = 1;
    inst->midi_channel = 1;
    strncpy(inst->instrument_name, "Untitled", sizeof(inst->instrument_name) - 1);

    /* Save module dir */
    if (module_dir) {
        strncpy(inst->module_dir, module_dir, sizeof(inst->module_dir) - 1);
    }

    /* Allocate capture buffer (stereo interleaved) */
    inst->capture_buf = calloc(MAX_CAPTURE_FRAMES * 2, sizeof(int16_t));
    if (!inst->capture_buf) {
        if (g_host && g_host->log)
            g_host->log("SampleRobot: FAILED to allocate capture buffer!");
        free(inst);
        return NULL;
    }

    if (g_host && g_host->log) {
        char msg[128];
        snprintf(msg, sizeof(msg), "SampleRobot: created instance, capture buf %d MB",
                 (int)((MAX_CAPTURE_FRAMES * 2 * sizeof(int16_t)) / (1024*1024)));
        g_host->log(msg);
    }

    inst->state = STATE_IDLE;
    snprintf(inst->status_text, sizeof(inst->status_text), "Ready");

    return inst;
}

static void sr_destroy_instance(void *instance) {
    samplerobot_instance_t *inst = instance;
    if (!inst) return;
    if (inst->capture_buf) free(inst->capture_buf);
    free(inst);
}

static void sr_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    /* No MIDI input handling needed */
    (void)instance; (void)msg; (void)len; (void)source;
}

/* Simple JSON value extractor: finds "key": value or "key": "string" */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        int i = 0;
        while (*p && *p != '"' && i < out_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return 1;
    } else {
        /* Number */
        int i = 0;
        while (*p && *p != ',' && *p != '}' && i < out_len - 1) {
            out[i++] = *p++;
        }
        out[i] = '\0';
        return 1;
    }
}

static void parse_config_json(samplerobot_instance_t *inst, const char *json) {
    char buf[128];

    if (json_get_string(json, "range_low", buf, sizeof(buf)))
        inst->range_low = atoi(buf);
    if (json_get_string(json, "range_high", buf, sizeof(buf)))
        inst->range_high = atoi(buf);
    if (json_get_string(json, "key_zones", buf, sizeof(buf))) {
        inst->key_zones = atoi(buf);
        if (inst->key_zones < 1) inst->key_zones = 1;
        if (inst->key_zones > SFZ_MAX_ZONES) inst->key_zones = SFZ_MAX_ZONES;
    }
    if (json_get_string(json, "velocity_layers", buf, sizeof(buf))) {
        inst->velocity_layers = atoi(buf);
        if (inst->velocity_layers < 1) inst->velocity_layers = 1;
        if (inst->velocity_layers > SFZ_MAX_LAYERS) inst->velocity_layers = SFZ_MAX_LAYERS;
    }
    if (json_get_string(json, "hold_duration", buf, sizeof(buf)))
        inst->hold_duration = (float)atof(buf);
    if (json_get_string(json, "loop_detect", buf, sizeof(buf)))
        inst->loop_detect = atoi(buf);
    if (json_get_string(json, "midi_channel", buf, sizeof(buf))) {
        inst->midi_channel = atoi(buf);
        if (inst->midi_channel < 1) inst->midi_channel = 1;
        if (inst->midi_channel > 16) inst->midi_channel = 16;
    }
    if (json_get_string(json, "instrument_name", buf, sizeof(buf))) {
        strncpy(inst->instrument_name, buf, sizeof(inst->instrument_name) - 1);
        inst->instrument_name[sizeof(inst->instrument_name) - 1] = '\0';
    }

    if (g_host && g_host->log) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "SampleRobot: parsed config - name='%s' range=%d-%d zones=%d layers=%d hold=%.1f loop=%d ch=%d",
                 inst->instrument_name, inst->range_low, inst->range_high,
                 inst->key_zones, inst->velocity_layers, inst->hold_duration,
                 inst->loop_detect, inst->midi_channel);
        g_host->log(msg);
    }
}

static void sr_set_param(void *instance, const char *key, const char *val) {
    samplerobot_instance_t *inst = instance;
    if (!inst || !key || !val) return;

    if (g_host && g_host->log) {
        char msg[512];
        snprintf(msg, sizeof(msg), "SampleRobot: set_param key='%s' val='%.200s'", key, val);
        g_host->log(msg);
    }

    if (strcmp(key, "config_json") == 0) {
        parse_config_json(inst, val);
        /* Check if start was included in the JSON */
        char buf[16];
        if (json_get_string(val, "start", buf, sizeof(buf)) && atoi(buf)) {
            if (inst->state == STATE_IDLE || inst->state == STATE_DONE) {
                if (g_host && g_host->log) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "SampleRobot: START (from config_json) name='%s' zones=%d layers=%d",
                             inst->instrument_name, inst->key_zones, inst->velocity_layers);
                    g_host->log(msg);
                }
                compute_schedule(inst);
                start_sampling(inst);
            }
        }
        return;
    } else if (strcmp(key, "range_low") == 0) {
        inst->range_low = atoi(val);
    } else if (strcmp(key, "range_high") == 0) {
        inst->range_high = atoi(val);
    } else if (strcmp(key, "key_zones") == 0) {
        inst->key_zones = atoi(val);
        if (inst->key_zones < 1) inst->key_zones = 1;
        if (inst->key_zones > SFZ_MAX_ZONES) inst->key_zones = SFZ_MAX_ZONES;
    } else if (strcmp(key, "velocity_layers") == 0) {
        inst->velocity_layers = atoi(val);
        if (inst->velocity_layers < 1) inst->velocity_layers = 1;
        if (inst->velocity_layers > SFZ_MAX_LAYERS) inst->velocity_layers = SFZ_MAX_LAYERS;
    } else if (strcmp(key, "hold_duration") == 0) {
        inst->hold_duration = (float)atof(val);
    } else if (strcmp(key, "loop_detect") == 0) {
        inst->loop_detect = atoi(val);
    } else if (strcmp(key, "midi_channel") == 0) {
        inst->midi_channel = atoi(val);
        if (inst->midi_channel < 1) inst->midi_channel = 1;
        if (inst->midi_channel > 16) inst->midi_channel = 16;
    } else if (strcmp(key, "instrument_name") == 0) {
        strncpy(inst->instrument_name, val, sizeof(inst->instrument_name) - 1);
        inst->instrument_name[sizeof(inst->instrument_name) - 1] = '\0';
    } else if (strcmp(key, "start") == 0) {
        if (inst->state == STATE_IDLE || inst->state == STATE_DONE) {
            if (g_host && g_host->log) {
                char msg[256];
                snprintf(msg, sizeof(msg), "SampleRobot: START - state=%d name='%s' zones=%d layers=%d",
                         inst->state, inst->instrument_name, inst->key_zones, inst->velocity_layers);
                g_host->log(msg);
            }
            compute_schedule(inst);
            start_sampling(inst);
            if (g_host && g_host->log) {
                char msg[256];
                snprintf(msg, sizeof(msg), "SampleRobot: schedule computed, total=%d, state=%d",
                         inst->total_samples, inst->state);
                g_host->log(msg);
            }
        }
    } else if (strcmp(key, "stop") == 0) {
        if (inst->state != STATE_IDLE && inst->state != STATE_DONE) {
            /* Send note off in case we're mid-sample */
            if (inst->state == STATE_RECORDING || inst->state == STATE_RELEASE) {
                send_note_off(inst);
            }
            /* If we captured any samples, generate partial SFZ */
            if (inst->completed_samples > 0) {
                finish_instrument(inst);
                snprintf(inst->status_text, sizeof(inst->status_text),
                         "Stopped. %d/%d samples captured.",
                         inst->completed_samples, inst->total_samples);
            } else {
                snprintf(inst->status_text, sizeof(inst->status_text), "Stopped.");
            }
            inst->state = STATE_DONE;
        }
    }
}

static int sr_get_param(void *instance, const char *key, char *buf, int buf_len) {
    samplerobot_instance_t *inst = instance;
    if (!inst || !key || !buf || buf_len <= 0) return 0;

    if (strcmp(key, "state") == 0) {
        const char *names[] = {"idle", "pre_sample", "recording", "release", "processing", "done"};
        snprintf(buf, buf_len, "%s", names[inst->state]);
    } else if (strcmp(key, "progress") == 0) {
        snprintf(buf, buf_len, "%d/%d",
                 inst->completed_samples + inst->skipped_samples, inst->total_samples);
    } else if (strcmp(key, "current_note") == 0) {
        if (inst->current_zone < inst->key_zones)
            snprintf(buf, buf_len, "%d", inst->sample_notes[inst->current_zone]);
        else
            snprintf(buf, buf_len, "0");
    } else if (strcmp(key, "current_velocity") == 0) {
        if (inst->current_layer < inst->velocity_layers)
            snprintf(buf, buf_len, "%d", inst->sample_velocities[inst->current_layer]);
        else
            snprintf(buf, buf_len, "0");
    } else if (strcmp(key, "current_note_name") == 0) {
        if (inst->current_zone < inst->key_zones) {
            int note = inst->sample_notes[inst->current_zone];
            int note_idx = note % 12;
            int octave = (note / 12) - 1;
            snprintf(buf, buf_len, "%s%d", NOTE_DISPLAY[note_idx], octave);
        } else {
            snprintf(buf, buf_len, "--");
        }
    } else if (strcmp(key, "status") == 0) {
        snprintf(buf, buf_len, "%s", inst->status_text);
    } else if (strcmp(key, "loops_found") == 0) {
        snprintf(buf, buf_len, "%d", inst->loops_found);
    } else if (strcmp(key, "completed") == 0) {
        snprintf(buf, buf_len, "%d", inst->completed_samples);
    } else if (strcmp(key, "total") == 0) {
        snprintf(buf, buf_len, "%d", inst->total_samples);
    } else if (strcmp(key, "skipped") == 0) {
        snprintf(buf, buf_len, "%d", inst->skipped_samples);
    } else if (strcmp(key, "range_low") == 0) {
        snprintf(buf, buf_len, "%d", inst->range_low);
    } else if (strcmp(key, "range_high") == 0) {
        snprintf(buf, buf_len, "%d", inst->range_high);
    } else if (strcmp(key, "key_zones") == 0) {
        snprintf(buf, buf_len, "%d", inst->key_zones);
    } else if (strcmp(key, "velocity_layers") == 0) {
        snprintf(buf, buf_len, "%d", inst->velocity_layers);
    } else if (strcmp(key, "hold_duration") == 0) {
        snprintf(buf, buf_len, "%.1f", inst->hold_duration);
    } else if (strcmp(key, "loop_detect") == 0) {
        snprintf(buf, buf_len, "%d", inst->loop_detect);
    } else if (strcmp(key, "midi_channel") == 0) {
        snprintf(buf, buf_len, "%d", inst->midi_channel);
    } else if (strcmp(key, "instrument_name") == 0) {
        snprintf(buf, buf_len, "%s", inst->instrument_name);
    } else if (strcmp(key, "midi_pending") == 0) {
        /* Format: "type,note,vel,ch" where type=0(none),1(on),2(off) */
        snprintf(buf, buf_len, "%d,%d,%d,%d",
                 inst->midi_pending, inst->midi_note,
                 inst->midi_vel, inst->midi_ch);
        /* Auto-clear after read */
        inst->midi_pending = 0;
    } else {
        return 0;
    }
    return 1;
}

static int sr_get_error(void *instance, char *buf, int buf_len) {
    (void)instance; (void)buf; (void)buf_len;
    return 0;
}

static void sr_render_block(void *instance, int16_t *out, int frames) {
    samplerobot_instance_t *inst = instance;
    /* Shim copies hardware audio_in before render_block, so this is real input */
    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    /* Always passthrough for monitoring */
    memcpy(out, audio_in, frames * 2 * sizeof(int16_t));

    /* Log first few render calls to confirm it's running */
    static int render_log_count = 0;
    if (inst->state != STATE_IDLE && inst->state != STATE_DONE && render_log_count < 5) {
        char msg[128];
        snprintf(msg, sizeof(msg), "SampleRobot: render_block state=%d block=%d capture=%d",
                 inst->state, inst->block_counter, inst->capture_frames);
        if (g_host && g_host->log) g_host->log(msg);
        render_log_count++;
    }

    switch (inst->state) {
    case STATE_IDLE:
    case STATE_DONE:
        break;

    case STATE_PRE_SAMPLE:
        /* Capture ~100ms for noise floor */
        append_audio(inst, audio_in, frames);
        inst->block_counter++;
        /* Log audio levels on first block of first sample */
        if (inst->block_counter == 1 && inst->current_zone == 0 && inst->current_layer == 0) {
            int16_t peak = 0;
            for (int i = 0; i < frames * 2; i++) {
                int16_t s = audio_in[i] < 0 ? -audio_in[i] : audio_in[i];
                if (s > peak) peak = s;
            }
            if (g_host && g_host->log) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "SampleRobot: audio_in check - src=%s peak=%d first4=[%d,%d,%d,%d]",
                         "mapped_mem",
                         peak, audio_in[0], audio_in[1], audio_in[2], audio_in[3]);
                g_host->log(msg);
            }
        }
        if (inst->block_counter >= 35) {  /* ~100ms at 128 frames/block */
            inst->noise_floor_rms = silence_rms(inst->capture_buf, inst->capture_frames);
            if (g_host && g_host->log) {
                char msg[128];
                snprintf(msg, sizeof(msg), "SampleRobot: noise_floor_rms=%.2f", inst->noise_floor_rms);
                g_host->log(msg);
            }
            inst->capture_frames = 0;
            send_note_on(inst);
            inst->state = STATE_RECORDING;
            inst->block_counter = 0;
            snprintf(inst->status_text, sizeof(inst->status_text), "Recording...");
        }
        break;

    case STATE_RECORDING:
        append_audio(inst, audio_in, frames);
        inst->block_counter++;
        if (inst->block_counter >= inst->hold_blocks) {
            send_note_off(inst);
            inst->state = STATE_RELEASE;
            inst->block_counter = 0;
            snprintf(inst->status_text, sizeof(inst->status_text), "Release...");
        }
        break;

    case STATE_RELEASE:
        append_audio(inst, audio_in, frames);
        inst->block_counter++;
        {
            /* Check silence: RMS of last 200ms below threshold */
            int window = 8820; /* 200ms */
            float threshold = inst->noise_floor_rms * 2.0f; /* noise_floor + ~6dB */
            int timeout_blocks = (int)(10.0f * MOVE_SAMPLE_RATE / MOVE_FRAMES_PER_BLOCK);

            if (inst->capture_frames >= window) {
                float tail_rms = silence_rms(
                    inst->capture_buf + (inst->capture_frames - window) * 2,
                    window);
                if (tail_rms < threshold || inst->block_counter >= timeout_blocks) {
                    inst->state = STATE_PROCESSING;
                    snprintf(inst->status_text, sizeof(inst->status_text), "Processing...");
                }
            }
            if (inst->block_counter >= timeout_blocks) {
                inst->state = STATE_PROCESSING;
                snprintf(inst->status_text, sizeof(inst->status_text), "Processing...");
            }
        }
        break;

    case STATE_PROCESSING:
        process_and_advance(inst);
        break;
    }
}

static plugin_api_v2_t g_plugin_api = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = sr_create_instance,
    .destroy_instance = sr_destroy_instance,
    .on_midi = sr_on_midi,
    .set_param = sr_set_param,
    .get_param = sr_get_param,
    .get_error = sr_get_error,
    .render_block = sr_render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_plugin_api;
}
