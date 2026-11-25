// kfb24_adpcm.h —— Fixed-block IMA ADPCM for the kfb24 audio uplink.
//
// One block is 640 PCM samples (40 ms at 16 kHz mono s16le). The encoder emits
// a 3-byte block header followed by 4-bit codes, 323 bytes total:
//
//   byte 0..1  first sample, int16 little-endian
//   byte 2     step index, 0..88
//   byte 3..   code n in the high nibble, code n+1 in the low nibble
//
// Every block is independent, so the bridge can drop a corrupted block without
// poisoning the following audio. The predictor is the block's first sample and
// is stored in the header. The adaptive step index is also stored per block;
// passing the previous block's state into the encoder keeps the first samples
// of each block from being quantized with a cold, tiny step. The PC bridge
// implements the same algorithm; keep both implementations and
// tests/test_kfb24_adpcm.c in sync.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define KFB24_ADPCM_SAMPLE_RATE 16000
#define KFB24_ADPCM_SAMPLES 640
#define KFB24_ADPCM_BYTES (3 + KFB24_ADPCM_SAMPLES / 2)

typedef struct {
    uint8_t index;  // step index to start the next block with
} kfb24_adpcm_enc_t;

// Encode exactly KFB24_ADPCM_SAMPLES samples. Returns the encoded byte count
// (KFB24_ADPCM_BYTES) or 0 when the input, output, or capacity is wrong.
// state may be NULL; otherwise it is updated for the next block.
size_t kfb24_adpcm_encode(const int16_t *pcm, size_t samples, uint8_t *out,
                          size_t cap, kfb24_adpcm_enc_t *state);

// Decode one encoded block into exactly KFB24_ADPCM_SAMPLES samples. Returns
// the sample count or 0 for a short block, a bad step index, or small output.
size_t kfb24_adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm,
                          size_t cap);
