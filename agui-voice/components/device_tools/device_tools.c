#include "device_tools.h"
#include "esp_log.h"

static const char *TAG = "device_tools";

// Skeleton stubs — implemented in P5 (ambient context) and P7 (set_timer/set_alarm/show_qr).

esp_err_t device_tools_init(void)
{
    ESP_LOGI(TAG, "init (stub)");
    return ESP_OK;
}

cJSON *device_context_build(void)
{
    return NULL;  // P5: motion + battery + local_time
}

cJSON *device_tools_manifest(void)
{
    return NULL;  // P7: JSON-schema tool list
}

void device_tools_register(const char *name, const cJSON *schema, device_tool_fn fn)
{
    (void)schema; (void)fn;
    ESP_LOGW(TAG, "device_tools_register(%s) not implemented yet", name ? name : "(null)");
}

esp_err_t device_tools_dispatch(const char *name, const cJSON *args, cJSON **result)
{
    (void)args; (void)result;
    ESP_LOGW(TAG, "device_tools_dispatch(%s) not implemented yet", name ? name : "(null)");
    return ESP_ERR_NOT_FOUND;
}
