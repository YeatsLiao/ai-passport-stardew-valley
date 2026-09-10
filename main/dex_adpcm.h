/* IMA ADPCM 4-bit decoder — ~50 lines, zero dependencies.
 * Decodes mono IMA ADPCM (format 0x11) to signed 16-bit PCM.
 * Reference: IMA ADPCM specification, Microsoft WAV format. */
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode one ADPCM block (header + compressed data) to PCM samples.
 * adpcm:      pointer to ADPCM block (starts with 4-byte block header)
 * adpcm_len:  bytes remaining in this block (including header)
 * pcm_out:    output buffer for 16-bit signed samples
 * pcm_cap:    capacity of pcm_out in samples (not bytes)
 * Returns:    number of samples written to pcm_out */
size_t dex_adpcm_decode_block(const uint8_t *adpcm, size_t adpcm_len,
                              int16_t *pcm_out, size_t pcm_cap);

/* Step tables for IMA ADPCM */
extern const int16_t dex_adpcm_step_table[89];
extern const int8_t  dex_adpcm_index_table[16];

#ifdef __cplusplus
}
#endif
