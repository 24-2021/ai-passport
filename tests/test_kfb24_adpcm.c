// Host tests for the kfb24 IMA ADPCM blocks: no ESP-IDF required.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kfb24_adpcm.h"

// Encoded with the board encoder for the deterministic block below. The PC
// bridge must decode this to the same PCM; do not regenerate this vector
// without updating both implementations.
static const uint8_t k_vector[KFB24_ADPCM_BYTES] = {
    0xf0, 0xd8, 0x00, 0x77, 0x77, 0x77, 0x79, 0x6a, 0x3b, 0xb5, 0xa3, 0xb4,
    0xa4, 0xb4, 0xa3, 0xba, 0x4b, 0x4a, 0x3b, 0x5a, 0x3b, 0x4a, 0xb4, 0xa3,
    0xb5, 0xa3, 0xb4, 0xa3, 0xbb, 0x5a, 0x3b, 0x4a, 0x4b, 0x4a, 0x3b, 0xa4,
    0xb4, 0xa3, 0xb5, 0xa3, 0xb4, 0xab, 0x4a, 0x3b, 0x5a, 0x3b, 0x4a, 0x3b,
    0xb5, 0xa3, 0xb4, 0xa4, 0xb4, 0xa3, 0xba, 0x4b, 0x4a, 0x3b, 0x5a, 0x3b,
    0x4a, 0xb4, 0xa3, 0xb5, 0xa3, 0xb4, 0xa3, 0xbb, 0x5a, 0x3b, 0x5a, 0x3b,
    0x4a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa4, 0xb4, 0xa3, 0xba, 0x4b, 0x4a, 0x3b,
    0x5a, 0x3b, 0x4a, 0xb4, 0xa3, 0xb5, 0xa3, 0xb4, 0xa3, 0xbb, 0x5a, 0x3b,
    0x4a, 0x4b, 0x4a, 0x3b, 0xa4, 0xb4, 0xa3, 0xb5, 0xa3, 0xb4, 0xff, 0xa8,
    0x19, 0x19, 0x18, 0x19, 0x19, 0x92, 0xa3, 0xa3, 0xb4, 0xa4, 0xb4, 0xaa,
    0x3b, 0x4a, 0x4b, 0x4a, 0x3b, 0x4a, 0xb4, 0xa3, 0xa4, 0xb4, 0xa3, 0xb5,
    0xab, 0x4a, 0x3b, 0x4a, 0x3a, 0x4b, 0x4a, 0xb4, 0xa3, 0xb5, 0xa3, 0xb4,
    0xa3, 0xbb, 0x5a, 0x3b, 0x4a, 0x4b, 0x4a, 0x3b, 0xa4, 0xb4, 0xa3, 0xb5,
    0xa3, 0xb4, 0xab, 0x4a, 0x3b, 0x5a, 0x3b, 0x4a, 0x3b, 0xb5, 0xa3, 0xb5,
    0xa3, 0xb4, 0xa3, 0xbb, 0x5a, 0x3b, 0x4a, 0x4b, 0x4a, 0x3b, 0xa4, 0xb4,
    0xa3, 0xb5, 0xa3, 0xb4, 0xab, 0x4a, 0x3b, 0x5a, 0x3b, 0x4a, 0x3b, 0xb5,
    0xa3, 0xb4, 0xa4, 0xb4, 0xa3, 0xba, 0x4b, 0x4a, 0x3b, 0x5a, 0x3b, 0x4a,
    0xb4, 0xa3, 0xb5, 0xa3, 0xb4, 0xff, 0xa8, 0x19, 0x19, 0x18, 0x19, 0x19,
    0x29, 0xa2, 0xa4, 0xa3, 0xb4, 0xa3, 0xb4, 0xab, 0x4a, 0x3b, 0x5a, 0x3b,
    0x5a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa3, 0xb5, 0xa3, 0xbb, 0x5a, 0x3b, 0x4a,
    0x3b, 0x5a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa3, 0xb5, 0xa3, 0xbb, 0x5a, 0x3b,
    0x4a, 0x3b, 0x5a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa3, 0xb5, 0xa3, 0xbb, 0x5a,
    0x3b, 0x4a, 0x3b, 0x5a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa3, 0xb5, 0xa3, 0xbb,
    0x5a, 0x3b, 0x4a, 0x3b, 0x5a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa3, 0xb5, 0xa3,
    0xbb, 0x5a, 0x3b, 0x4a, 0x3b, 0x5a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa3, 0xb5,
    0xa3, 0xbb, 0x5a, 0x3b, 0x4a, 0x3b, 0x5a, 0x3b, 0xb5, 0xa3, 0xb4, 0xa3,
    0xff, 0xb0, 0x89, 0x18, 0x19, 0x19, 0x29, 0x2a, 0x3a, 0xb4, 0xb0,
};

static uint32_t crc32_le(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static void fill_block(int16_t *pcm, int block) {
    for (int i = 0; i < KFB24_ADPCM_SAMPLES; i++) {
        int global = block * KFB24_ADPCM_SAMPLES + i;
        int32_t v = (global * 97) % 20000 - 10000;
        v += ((global * 7) % 13) * 300;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
    }
}

static void test_fixed_vector(void) {
    int16_t pcm[KFB24_ADPCM_SAMPLES];
    size_t n = kfb24_adpcm_decode(k_vector, sizeof(k_vector), pcm,
                                  KFB24_ADPCM_SAMPLES);
    assert(n == KFB24_ADPCM_SAMPLES);
    assert(pcm[0] == -10000);
    assert(crc32_le((const uint8_t *)pcm, sizeof(pcm)) == 0x6dc7fb08u);
}

static void test_round_trip(void) {
    int16_t pcm[KFB24_ADPCM_SAMPLES];
    int16_t decoded[KFB24_ADPCM_SAMPLES];
    uint8_t out[KFB24_ADPCM_BYTES];

    memset(pcm, 0, sizeof(pcm));
    size_t n = kfb24_adpcm_encode(pcm, KFB24_ADPCM_SAMPLES, out, sizeof(out), NULL);
    assert(n == KFB24_ADPCM_BYTES);
    n = kfb24_adpcm_decode(out, n, decoded, KFB24_ADPCM_SAMPLES);
    assert(n == KFB24_ADPCM_SAMPLES);
    for (int i = 0; i < KFB24_ADPCM_SAMPLES; i++) assert(decoded[i] == 0);

    fill_block(pcm, 0);
    n = kfb24_adpcm_encode(pcm, KFB24_ADPCM_SAMPLES, out, sizeof(out), NULL);
    assert(n == KFB24_ADPCM_BYTES);
    assert(memcmp(out, k_vector, sizeof(k_vector)) == 0);
    n = kfb24_adpcm_decode(out, n, decoded, KFB24_ADPCM_SAMPLES);
    assert(n == KFB24_ADPCM_SAMPLES);
    assert(crc32_le((const uint8_t *)decoded, sizeof(decoded)) == 0x6dc7fb08u);
}

// Smooth 100 Hz triangle: the block boundaries land on a steep slope, so a
// cold step index is clearly worse than the carried one.
static void fill_triangle(int16_t *pcm, int block) {
    const int period = 160;
    for (int i = 0; i < KFB24_ADPCM_SAMPLES; i++) {
        int global = block * KFB24_ADPCM_SAMPLES + i;
        int phase = (global * 2) % (2 * period) - period;
        pcm[i] = (int16_t)((phase < 0 ? -phase : phase) * 200 - 12000);
    }
}

static long long encode_error(int16_t *pcm, uint8_t *out) {
    int16_t decoded[KFB24_ADPCM_SAMPLES];
    size_t n = kfb24_adpcm_decode(out, KFB24_ADPCM_BYTES, decoded,
                                  KFB24_ADPCM_SAMPLES);
    assert(n == KFB24_ADPCM_SAMPLES);
    long long error = 0;
    for (int i = 0; i < KFB24_ADPCM_SAMPLES; i++) {
        long long d = (long long)pcm[i] - decoded[i];
        error += d * d;
    }
    return error;
}

static void test_state_carry(void) {
    int16_t pcm[KFB24_ADPCM_SAMPLES];
    uint8_t out[KFB24_ADPCM_BYTES];
    kfb24_adpcm_enc_t state = {0};
    long long cold_error = 0;
    long long warm_error = 0;
    long long signal = 0;

    for (int block = 0; block < 25; block++) {
        fill_triangle(pcm, block);
        size_t n = kfb24_adpcm_encode(pcm, KFB24_ADPCM_SAMPLES, out,
                                      sizeof(out), NULL);
        assert(n == KFB24_ADPCM_BYTES);
        cold_error += encode_error(pcm, out);
        n = kfb24_adpcm_encode(pcm, KFB24_ADPCM_SAMPLES, out, sizeof(out),
                               &state);
        assert(n == KFB24_ADPCM_BYTES);
        warm_error += encode_error(pcm, out);
        for (int i = 0; i < KFB24_ADPCM_SAMPLES; i++) {
            signal += (long long)pcm[i] * pcm[i];
        }
    }
    assert(cold_error > 0 && warm_error > 0);
    assert(signal > warm_error * 1000);   // > 30 dB with the carried index
    assert(warm_error * 2 < cold_error);  // clearly better than cold starts
}

static void test_rejects_bad_input(void) {
    int16_t pcm[KFB24_ADPCM_SAMPLES] = {0};
    int16_t decoded[KFB24_ADPCM_SAMPLES];
    uint8_t out[KFB24_ADPCM_BYTES];
    uint8_t bad[KFB24_ADPCM_BYTES];

    assert(kfb24_adpcm_encode(NULL, KFB24_ADPCM_SAMPLES, out, sizeof(out), NULL) == 0);
    assert(kfb24_adpcm_encode(pcm, KFB24_ADPCM_SAMPLES - 1, out, sizeof(out), NULL) == 0);
    assert(kfb24_adpcm_encode(pcm, KFB24_ADPCM_SAMPLES, out, sizeof(out) - 1, NULL) == 0);

    memcpy(bad, k_vector, sizeof(bad));
    assert(kfb24_adpcm_decode(NULL, sizeof(bad), decoded, KFB24_ADPCM_SAMPLES) == 0);
    assert(kfb24_adpcm_decode(bad, sizeof(bad) - 1, decoded, KFB24_ADPCM_SAMPLES) == 0);
    assert(kfb24_adpcm_decode(bad, sizeof(bad), decoded, KFB24_ADPCM_SAMPLES - 1) == 0);

    bad[2] = 89;
    assert(kfb24_adpcm_decode(bad, sizeof(bad), decoded, KFB24_ADPCM_SAMPLES) == 0);
}

int main(void) {
    test_fixed_vector();
    test_round_trip();
    test_state_carry();
    test_rejects_bad_input();
    printf("test_kfb24_adpcm: PASS\n");
    return 0;
}
