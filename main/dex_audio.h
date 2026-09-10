/* BGM audio playback — ADPCM decode + I2S streaming.
 * Runs in a dedicated task; non-blocking API for UI layer. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize audio subsystem (bsp_audio). Call once at startup. */
void dex_audio_init(void);

/* Start playing a track by index (0..DEX_BGM_TRACK_COUNT-1).
 * Loops continuously until stopped. */
void dex_audio_play(int track_index);

/* Stop playback. */
void dex_audio_stop(void);

/* Toggle mute (keeps playing but silent). */
void dex_audio_toggle_mute(void);

/* Returns true if currently playing. */
bool dex_audio_is_playing(void);

#ifdef __cplusplus
}
#endif
