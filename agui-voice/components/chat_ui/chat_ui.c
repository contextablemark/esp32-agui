#include "chat_ui.h"
#include "esp_log.h"

static const char *TAG = "chat_ui";

// Skeleton stubs — implemented in P3 (chat list), P4 (status), P6 (interrupt + QR).

esp_err_t chat_ui_init(void)
{
    ESP_LOGI(TAG, "init (stub)");
    return ESP_OK;
}

void chat_ui_add_user(const char *text)            { (void)text; }
void *chat_ui_begin_assistant(void)                { return NULL; }
void chat_ui_append_assistant(const char *delta)   { (void)delta; }
void chat_ui_status(const char *text)              { (void)text; }
void chat_ui_clear_status(void)                    { }
void chat_ui_show_qr(const char *data)             { (void)data; }
void chat_ui_idle_timer(int s, const char *label)  { (void)s; (void)label; }

void chat_ui_prompt(const char *message, const cJSON *response_schema,
                    int64_t expires_at, chat_ui_answer_cb cb, void *ctx)
{
    (void)message; (void)response_schema; (void)expires_at; (void)cb; (void)ctx;
    ESP_LOGW(TAG, "chat_ui_prompt not implemented yet");
}
