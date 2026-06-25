// soniox_tts_client — spoken AG-UI replies via Soniox real-time TTS (wss://tts-rt.soniox.com).
//
// P-a (this version): BATCH. soniox_tts_speak(text) opens the TTS WSS AFTER the AG-UI run has
// finished (so only one TLS session is open at a time — "sequential TLS"), sends the whole reply +
// text_end, receives base64 pcm_s16le @16k/mono, plays it through a caller-provided sink, and closes.
// Proves the protocol/auth, the audio format (mono vs stereo-as-mono), the speaker write path, and
// that the TLS coexists on this RAM-tight build. Streaming + barge-in are P-b / P-c.
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// PCM sink: raw 16-bit/16k/mono samples to play. Provided by main (which owns the ES8311 OUT / s_spk),
// so there is a single speaker owner (the beep + low-power codec-close never race a second handle).
typedef void (*tts_pcm_sink_t)(const void *pcm, size_t bytes);

// One-time: register the playback sink + spin up the drain task. Call once at boot.
esp_err_t soniox_tts_init(tts_pcm_sink_t sink);

// Speak `text` and BLOCK until playback finishes (or error/timeout). Opens the TTS WSS, sends the
// text, plays the returned PCM, closes. Soniox key comes from NVS (app_cfg). Serialized internally.
esp_err_t soniox_tts_speak(const char *text);

// PTT barge-in: cancel an in-flight soniox_tts_speak() from any task (e.g. a button cb during
// playback). Returns immediately; audio stops within ~1 drain chunk plus the codec DMA tail, and the
// blocked soniox_tts_speak() returns shortly after. No-op when nothing is speaking.
void soniox_tts_cancel(void);

#ifdef __cplusplus
}
#endif
