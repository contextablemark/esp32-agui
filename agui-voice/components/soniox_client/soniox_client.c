#include "soniox_client.h"
#include "esp_log.h"

static const char *TAG = "soniox_client";

// Skeleton stubs — implemented in P1 (ES8311 capture → WSS → transcript).

esp_err_t soniox_client_init(void)
{
    ESP_LOGI(TAG, "init (stub)");
    return ESP_OK;
}

esp_err_t soniox_open(const soniox_cfg_t *cfg, soniox_token_cb on_token,
                      soniox_done_cb on_done, void *ctx)
{
    (void)cfg; (void)on_token; (void)on_done; (void)ctx;
    ESP_LOGW(TAG, "soniox_open not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t soniox_send_pcm(const int16_t *pcm, size_t samples)
{
    (void)pcm; (void)samples;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t soniox_finish(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void soniox_close(void)
{
    ESP_LOGW(TAG, "soniox_close not implemented yet");
}
