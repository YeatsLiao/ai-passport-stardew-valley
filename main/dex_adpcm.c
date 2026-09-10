/* IMA ADPCM 4-bit decoder implementation.
 * Reference: IMA ADPCM specification. */
#include "dex_adpcm.h"

/* Step size table: 89 entries, indexed by adapter index */
const int16_t dex_adpcm_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

/* Index adjustment table: indexed by 4-bit ADPCM code (0-15) */
const int8_t dex_adpcm_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

size_t dex_adpcm_decode_block(const uint8_t *adpcm, size_t adpcm_len,
                              int16_t *pcm_out, size_t pcm_cap)
{
    if (adpcm_len < 4 || pcm_cap == 0) return 0;

    /* Block header: predictor (16-bit signed) + step index (8-bit) + reserved */
    int16_t predictor = (int16_t)(adpcm[0] | (adpcm[1] << 8));
    int step_index = adpcm[2];
    /* adpcm[3] is reserved/padding */

    if (step_index < 0) step_index = 0;
    if (step_index > 88) step_index = 88;

    size_t samples_written = 0;
    const uint8_t *src = adpcm + 4;  /* skip header */
    size_t src_len = adpcm_len - 4;

    /* Output the first sample (the predictor from header) */
    if (samples_written < pcm_cap) {
        pcm_out[samples_written++] = predictor;
    }

    /* Decode 4-bit ADPCM samples (2 per byte, low nibble first) */
    for (size_t i = 0; i < src_len && samples_written < pcm_cap; i++) {
        uint8_t byte = src[i];

        /* Decode low nibble */
        {
            int code = byte & 0x0F;
            int step = dex_adpcm_step_table[step_index];
            int diff = step >> 3;
            if (code & 1) diff += step >> 2;
            if (code & 2) diff += step >> 1;
            if (code & 4) diff += step;
            if (code & 8) diff = -diff;

            predictor += diff;
            if (predictor > 32767) predictor = 32767;
            if (predictor < -32768) predictor = -32768;

            if (samples_written < pcm_cap) {
                pcm_out[samples_written++] = (int16_t)predictor;
            }

            step_index += dex_adpcm_index_table[code];
            if (step_index < 0) step_index = 0;
            if (step_index > 88) step_index = 88;
        }

        /* Decode high nibble */
        if (samples_written < pcm_cap) {
            int code = (byte >> 4) & 0x0F;
            int step = dex_adpcm_step_table[step_index];
            int diff = step >> 3;
            if (code & 1) diff += step >> 2;
            if (code & 2) diff += step >> 1;
            if (code & 4) diff += step;
            if (code & 8) diff = -diff;

            predictor += diff;
            if (predictor > 32767) predictor = 32767;
            if (predictor < -32768) predictor = -32768;

            pcm_out[samples_written++] = (int16_t)predictor;

            step_index += dex_adpcm_index_table[code];
            if (step_index < 0) step_index = 0;
            if (step_index > 88) step_index = 88;
        }
    }

    return samples_written;
}
