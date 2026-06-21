// soniox_client — real-time streaming STT over WebSocket (Soniox).
//
// Config frame (model stt-rt-v5, pcm s16le 16k mono, endpoint detection) → binary PCM
// frames → empty frame to end. Mic capture ref: app-pixels/ai-chat audio_engine.cpp
// (16 kHz mono, stereo→mono downmix). See docs/agui-voice-plan.md §5.1 / §7.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *endpoint;     // wss://stt-rt.soniox.com/transcribe-websocket
    const char *api_key;      // ephemeral key preferred
    const char *model;        // "stt-rt-v5"
    int  sample_rate;         // 16000
    int  channels;            // 1
    bool endpoint_detection;  // true
} soniox_cfg_t;

typedef void (*soniox_token_cb)(const char *text, bool is_final, void *ctx);
typedef void (*soniox_done_cb)(const char *utterance, void *ctx);  // endpoint/finished

// One-time init.
esp_err_t soniox_client_init(void);

esp_err_t soniox_open(const soniox_cfg_t *cfg, soniox_token_cb on_token,
                      soniox_done_cb on_done, void *ctx);
esp_err_t soniox_send_pcm(const int16_t *pcm, size_t samples);  // binary frame
esp_err_t soniox_finish(void);                                  // empty frame
void      soniox_close(void);

#ifdef __cplusplus
}
#endif
