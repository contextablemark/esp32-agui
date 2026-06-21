// AG-UI on-device voice client — application entry point.
// Skeleton (P0): boots, inits NVS, wires the component init stubs so the build graph
// is complete. Each component is fleshed out across phases P0..P8 — see
// docs/agui-voice-plan.md for the full plan and the per-component API sketches.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "net_prov.h"
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

void app_main(void)
{
    ESP_LOGI(TAG, "AG-UI voice client booting (P0/P0.5)");
    init_nvs();

    // Bring up components. Order: UI/tools first (no network), then networking.
    device_tools_init();
    chat_ui_init();
    soniox_client_init();
    agui_client_init();

    // P0/P0.5 — WiFi: try saved networks; if none/fail, open the captive portal and
    // wait for the user to provision from a phone, then retry. (docs/agui-voice-plan.md)
    ESP_ERROR_CHECK(net_prov_init());
    while (net_connect_saved(15000) != ESP_OK) {
        ESP_LOGW(TAG, "no WiFi — opening captive portal 'AMOLED-setup'");
        net_portal_start("AMOLED-setup");
        net_portal_wait_saved(0);   // block until credentials submitted
        net_portal_stop();
    }
    net_start_auto_reconnect();
    ESP_LOGI(TAG, "network ready — awaiting later-phase implementations (P1+)");

    // Heartbeat: keeps the USB-Serial/JTAG console showing live output regardless of when
    // the host terminal (re)connects, and reports link/IP/heap. Removed once the UI exists.
    char ip[16];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (net_get_ip_str(ip, sizeof(ip))) {
            ESP_LOGI(TAG, "heartbeat: online ip=%s  free_heap=%u  psram=%u",
                     ip, (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGW(TAG, "heartbeat: offline  free_heap=%u",
                     (unsigned)esp_get_free_heap_size());
        }
    }
}
