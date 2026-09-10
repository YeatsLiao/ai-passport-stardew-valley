/* BGM audio playback implementation.
 * Decodes IMA ADPCM blocks to PCM and streams to I2S via bsp_audio. */
#include "dex_audio.h"
#include "dex_bgm_data.h"
#include "dex_adpcm.h"
#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "dex_audio";

/* PCM decode buffer: one ADPCM block (1024 bytes) -> up to 2041 samples */
#define PCM_BUF_SAMPLES 2048
static int16_t s_pcm_buf[PCM_BUF_SAMPLES];

/* Playback state */
static TaskHandle_t s_task = NULL;
static volatile bool s_playing = false;
static volatile bool s_muted = false;
static volatile int s_track_idx = -1;
static volatile bool s_stop_req = false;

static void audio_task(void *arg);

void dex_audio_init(void)
{
    ESP_ERROR_CHECK(bsp_audio_init());
    bsp_audio_set_format(8000, 16, 1);  /* 8kHz, 16-bit, mono */
    bsp_audio_set_volume(80);            /* 80% volume */
    ESP_LOGI(TAG, "audio init OK");
}

void dex_audio_play(int track_index)
{
    if (track_index < 0 || track_index >= DEX_BGM_TRACK_COUNT) {
        ESP_LOGW(TAG, "invalid track %d", track_index);
        return;
    }
    
    /* Stop current playback if any */
    if (s_task) {
        s_stop_req = true;
        vTaskDelay(pdMS_TO_TICKS(50));  /* Let task exit */
        if (s_task) {
            vTaskDelete(s_task);
            s_task = NULL;
        }
    }
    
    s_track_idx = track_index;
    s_stop_req = false;
    s_playing = true;
    s_muted = false;
    
    xTaskCreate(audio_task, "bgm_play", 4096, NULL, 5, &s_task);
    ESP_LOGI(TAG, "play track %d: %s", track_index, dex_bgm_tracks[track_index].name);
}

void dex_audio_stop(void)
{
    if (!s_playing) return;
    s_stop_req = true;
    s_playing = false;
    ESP_LOGI(TAG, "stop requested");
}

void dex_audio_toggle_mute(void)
{
    s_muted = !s_muted;
    ESP_LOGI(TAG, "mute: %s", s_muted ? "ON" : "OFF");
}

bool dex_audio_is_playing(void)
{
    return s_playing;
}

/* Decode and stream one ADPCM block, return samples decoded */
static size_t decode_block(const uint8_t *block, size_t block_len)
{
    return dex_adpcm_decode_block(block, block_len, s_pcm_buf, PCM_BUF_SAMPLES);
}

static void audio_task(void *arg)
{
    const dex_bgm_track_t *track = &dex_bgm_tracks[s_track_idx];
    const uint8_t *data = track->data;
    uint32_t data_len = track->len;
    uint16_t block_align = track->block_align;
    
    ESP_LOGI(TAG, "task start: %s, %lu bytes, block_align=%u",
             track->name, (unsigned long)data_len, block_align);
    
    /* Set audio format for this track */
    bsp_audio_set_format(track->sample_rate, 16, 1);
    
    uint32_t offset = 0;
    while (!s_stop_req) {
        /* Decode one block */
        if (offset + block_align > data_len) {
            /* End of track — loop */
            offset = 0;
            ESP_LOGD(TAG, "loop");
        }
        
        size_t samples = decode_block(data + offset, block_align);
        offset += block_align;
        
        if (samples > 0 && !s_muted) {
            /* Write PCM to I2S (blocking) */
            size_t bytes = samples * 2;  /* 16-bit = 2 bytes/sample */
            bsp_audio_write(s_pcm_buf, bytes);
        }
        
        /* Small yield to watchdog */
        taskYIELD();
    }
    
    ESP_LOGI(TAG, "task exit");
    s_playing = false;
    s_task = NULL;
    vTaskDelete(NULL);
}
