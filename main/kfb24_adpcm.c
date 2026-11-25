#include "kfb24_adpcm.h"

#include <string.h>

static const int8_t k_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int16_t k_step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static int16_t clamp_sample(int32_t value) {
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

static int clamp_index(int index) {
    if (index < 0) return 0;
    if (index > 88) return 88;
    return index;
}

size_t kfb24_adpcm_encode(const int16_t *pcm, size_t samples, uint8_t *out,
                          size_t cap, kfb24_adpcm_enc_t *state) {
    if (!pcm || !out || samples != KFB24_ADPCM_SAMPLES ||
        cap < KFB24_ADPCM_BYTES) {
        return 0;
    }

    int32_t predictor = pcm[0];
    int index = state ? state->index : 0;
    if (index > 88) index = 0;
    out[0] = (uint8_t)(predictor & 0xFF);
    out[1] = (uint8_t)((predictor >> 8) & 0xFF);
    out[2] = (uint8_t)index;

    size_t nibble = 3;
    for (size_t i = 1; i < samples; i++) {
        int step = k_step_table[index];
        int32_t diff = (int32_t)pcm[i] - predictor;
        uint8_t code = 0;
        if (diff < 0) {
            code = 8;
            diff = -diff;
        }

        int32_t delta = step >> 3;
        if (diff >= step) {
            code |= 4;
            diff -= step;
            delta += step;
        }
        step >>= 1;
        if (diff >= step) {
            code |= 2;
            diff -= step;
            delta += step;
        }
        step >>= 1;
        if (diff >= step) {
            code |= 1;
            delta += step;
        }

        predictor += (code & 8) ? -delta : delta;
        predictor = clamp_sample(predictor);
        index = clamp_index(index + k_index_table[code]);

        if ((i & 1) != 0) {
            out[nibble] = (uint8_t)(code << 4);
        } else {
            out[nibble] |= code;
            nibble++;
        }
    }
    if (state) state->index = (uint8_t)index;
    return KFB24_ADPCM_BYTES;
}

size_t kfb24_adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm,
                          size_t cap) {
    if (!in || !pcm || len < KFB24_ADPCM_BYTES || cap < KFB24_ADPCM_SAMPLES) {
        return 0;
    }

    int32_t predictor = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
    int index = in[2];
    if (index > 88) return 0;
    pcm[0] = (int16_t)predictor;

    size_t nibble = 3;
    for (size_t i = 1; i < KFB24_ADPCM_SAMPLES; i++) {
        uint8_t code;
        if ((i & 1) != 0) {
            code = (uint8_t)(in[nibble] >> 4);
        } else {
            code = (uint8_t)(in[nibble] & 0x0F);
            nibble++;
        }

        int step = k_step_table[index];
        int32_t delta = step >> 3;
        if (code & 4) delta += step;
        if (code & 2) delta += step >> 1;
        if (code & 1) delta += step >> 2;
        predictor += (code & 8) ? -delta : delta;
        predictor = clamp_sample(predictor);
        index = clamp_index(index + k_index_table[code]);
        pcm[i] = (int16_t)predictor;
    }
    return KFB24_ADPCM_SAMPLES;
}
