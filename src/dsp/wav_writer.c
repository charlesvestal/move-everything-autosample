#include "wav_writer.h"
#include <stdio.h>
#include <string.h>

/* Write a little-endian uint16 */
static int write_u16(FILE *f, uint16_t v) {
    uint8_t buf[2];
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    return fwrite(buf, 1, 2, f) == 2 ? 0 : -1;
}

/* Write a little-endian uint32 */
static int write_u32(FILE *f, uint32_t v) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)((v >> 8) & 0xFF);
    buf[2] = (uint8_t)((v >> 16) & 0xFF);
    buf[3] = (uint8_t)((v >> 24) & 0xFF);
    return fwrite(buf, 1, 4, f) == 4 ? 0 : -1;
}

/* Write a 4-byte tag (e.g. "RIFF", "fmt ", "data") */
static int write_tag(FILE *f, const char *tag) {
    return fwrite(tag, 1, 4, f) == 4 ? 0 : -1;
}

int wav_write(const char *path,
              const int16_t *interleaved_stereo,
              int num_frames,
              int loop_start,
              int loop_end) {
    if (!path || !interleaved_stereo || num_frames <= 0)
        return -1;

    int has_loop = (loop_start >= 0 && loop_end > loop_start);

    uint32_t num_channels = 2;
    uint32_t sample_rate = 44100;
    uint32_t bits_per_sample = 16;
    uint32_t bytes_per_sample = bits_per_sample / 8;
    uint32_t block_align = num_channels * bytes_per_sample;
    uint32_t byte_rate = sample_rate * block_align;

    uint32_t fmt_size = 16;
    uint32_t data_size = (uint32_t)num_frames * num_channels * bytes_per_sample;
    uint32_t smpl_size = 60; /* 9 uint32 header + 6 uint32 loop = 60 bytes */

    /* RIFF chunk size = 4 ("WAVE") + (8 + fmt_size) + (8 + data_size) [+ (8 + smpl_size)] */
    uint32_t riff_size = 4 + (8 + fmt_size) + (8 + data_size);
    if (has_loop)
        riff_size += (8 + smpl_size);

    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    /* RIFF header */
    if (write_tag(f, "RIFF") < 0) goto fail;
    if (write_u32(f, riff_size) < 0) goto fail;
    if (write_tag(f, "WAVE") < 0) goto fail;

    /* fmt sub-chunk */
    if (write_tag(f, "fmt ") < 0) goto fail;
    if (write_u32(f, fmt_size) < 0) goto fail;
    if (write_u16(f, 1) < 0) goto fail;              /* PCM format */
    if (write_u16(f, (uint16_t)num_channels) < 0) goto fail;
    if (write_u32(f, sample_rate) < 0) goto fail;
    if (write_u32(f, byte_rate) < 0) goto fail;
    if (write_u16(f, (uint16_t)block_align) < 0) goto fail;
    if (write_u16(f, (uint16_t)bits_per_sample) < 0) goto fail;

    /* data sub-chunk */
    if (write_tag(f, "data") < 0) goto fail;
    if (write_u32(f, data_size) < 0) goto fail;

    /* Write PCM samples - convert to little-endian explicitly */
    for (int i = 0; i < num_frames * (int)num_channels; i++) {
        if (write_u16(f, (uint16_t)interleaved_stereo[i]) < 0) goto fail;
    }

    /* Optional smpl chunk for loop points */
    if (has_loop) {
        if (write_tag(f, "smpl") < 0) goto fail;
        if (write_u32(f, smpl_size) < 0) goto fail;

        /* smpl header fields */
        if (write_u32(f, 0) < 0) goto fail;          /* manufacturer */
        if (write_u32(f, 0) < 0) goto fail;          /* product */
        if (write_u32(f, 22675) < 0) goto fail;      /* sample period (1e9/44100) */
        if (write_u32(f, 60) < 0) goto fail;         /* MIDI unity note */
        if (write_u32(f, 0) < 0) goto fail;          /* MIDI pitch fraction */
        if (write_u32(f, 0) < 0) goto fail;          /* SMPTE format */
        if (write_u32(f, 0) < 0) goto fail;          /* SMPTE offset */
        if (write_u32(f, 1) < 0) goto fail;          /* num sample loops */
        if (write_u32(f, 0) < 0) goto fail;          /* sampler data size */

        /* Loop definition */
        if (write_u32(f, 0) < 0) goto fail;          /* cue point ID */
        if (write_u32(f, 0) < 0) goto fail;          /* type: forward */
        if (write_u32(f, (uint32_t)loop_start) < 0) goto fail;
        if (write_u32(f, (uint32_t)loop_end) < 0) goto fail;
        if (write_u32(f, 0) < 0) goto fail;          /* fraction */
        if (write_u32(f, 0) < 0) goto fail;          /* play count */
    }

    fclose(f);
    return 0;

fail:
    fclose(f);
    return -1;
}
