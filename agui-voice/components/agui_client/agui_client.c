#include "agui_client.h"
#include "esp_log.h"

static const char *TAG = "agui_client";

// Skeleton stubs — implemented across P2 (text), P4 (status), P5 (context),
// P6 (interrupt/resume), P7 (client tools). See docs/agui-voice-plan.md §6.

esp_err_t agui_client_init(void)
{
    ESP_LOGI(TAG, "init (stub)");
    return ESP_OK;
}

esp_err_t agui_run(const agui_cfg_t *cfg, const char *user_text,
                   const cJSON *context, const cJSON *tools, const cJSON *resume,
                   const agui_handlers_t *handlers, void *ctx)
{
    (void)cfg; (void)user_text; (void)context; (void)tools;
    (void)resume; (void)handlers; (void)ctx;
    ESP_LOGW(TAG, "agui_run not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t agui_tool_result(const char *tool_call_id, const cJSON *result)
{
    (void)tool_call_id; (void)result;
    return ESP_ERR_NOT_SUPPORTED;
}
