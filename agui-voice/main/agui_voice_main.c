// AG-UI on-device voice client — application entry point.
// P0/P0.5: WiFi (multi-SSID + captive portal). P1: streaming STT (Soniox) — speak and
// watch the console for live transcripts. See docs/agui-voice-plan.md for the roadmap.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "net_prov.h"
#include "app_cfg.h"
#include "soniox_client.h"
#include "agui_client.h"
#include "device_tools.h"
#include "chat_ui.h"

static const char *TAG = "agui_voice";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// --- Soniox transcript callbacks (P1 test harness; fire from the websocket task) ---
static void on_partial(const char *text, void *ctx)
{
    ESP_LOGI("stt", "~ %s", text);          // live interim transcript
}
static void on_turn(const char *text, void *ctx)
{
    ESP_LOGI("stt", "TURN: %s", text);      // committed utterance (Soniox <end>)
}

void app_main(void)
{
    ESP_LOGI(TAG, "AG-UI voice client booting (P0/P0.5/P1)");
    init_nvs();

    // Non-network components (stubs until later phases).
    device_tools_init();
    chat_ui_init();
    agui_client_init();

    // Provision until we have BOTH WiFi and a Soniox API key. The portal collects either
    // (WiFi creds and/or the Soniox key); on first P1 boot WiFi is already saved but the
    // key isn't, so the portal opens just to capture the key.
    ESP_ERROR_CHECK(net_prov_init());
    for (;;) {
        bool wifi_ok = net_is_connected() || (net_connect_saved(15000) == ESP_OK);
        bool key_ok  = app_cfg_has(APP_CFG_SONIOX_KEY);
        if (wifi_ok && key_ok) break;
        ESP_LOGW(TAG, "provisioning needed (wifi=%d, soniox_key=%d) — opening 'AMOLED-setup'",
                 wifi_ok, key_ok);
        net_portal_start("AMOLED-setup");
        net_portal_wait_saved(0);   // block until the user submits the form
        net_portal_stop();
    }
    net_start_auto_reconnect();
    ESP_LOGI(TAG, "network + Soniox key ready");

    // P1: start streaming STT. Continuous capture; Soniox endpoint-detection emits a turn
    // ("<end>") per utterance. Speak into the mic and watch the 'stt' logs.
    if (soniox_client_init() == ESP_OK) {
        soniox_cfg_t scfg = { 0 };   // defaults; api_key read from NVS
        if (soniox_session_start(&scfg, on_partial, on_turn, NULL) == ESP_OK) {
            ESP_LOGI(TAG, "Soniox session started — speak into the mic");
        } else {
            ESP_LOGE(TAG, "Soniox session failed to start");
        }
    }

    // Heartbeat: keeps the USB-Serial/JTAG console showing live output and reports link/heap.
    char ip[16];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const char *stt_err = soniox_last_error();
        if (net_get_ip_str(ip, sizeof(ip))) {
            ESP_LOGI(TAG, "heartbeat: online ip=%s  stt=%d  free_heap=%u  psram=%u",
                     ip, soniox_session_active(), (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGW(TAG, "heartbeat: offline  free_heap=%u",
                     (unsigned)esp_get_free_heap_size());
        }
        if (stt_err) ESP_LOGE(TAG, "STT disabled — Soniox error: %s (re-check the key)", stt_err);
    }
}
