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

#define MOVE_PLUGIN_API_VERSION_2 2
#define MOVE_SAMPLE_RATE 44100
#define MOVE_FRAMES_PER_BLOCK 128

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

typedef struct {
    int dummy;
} samplerobot_instance_t;

static void* sr_create_instance(const char *module_dir, const char *json_defaults) {
    samplerobot_instance_t *inst = calloc(1, sizeof(samplerobot_instance_t));
    return inst;
}

static void sr_destroy_instance(void *instance) {
    free(instance);
}

static void sr_on_midi(void *instance, const uint8_t *msg, int len, int source) {
}

static void sr_set_param(void *instance, const char *key, const char *val) {
}

static int sr_get_param(void *instance, const char *key, char *buf, int buf_len) {
    return 0;
}

static int sr_get_error(void *instance, char *buf, int buf_len) {
    return 0;
}

static void sr_render_block(void *instance, int16_t *out, int frames) {
    /* Passthrough: copy audio_in to audio_out for monitoring */
    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);
    memcpy(out, audio_in, frames * 2 * sizeof(int16_t));
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
