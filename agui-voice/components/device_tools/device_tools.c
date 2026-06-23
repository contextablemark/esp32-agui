#include "device_tools.h"

#include <time.h>

#include "esp_log.h"

static const char *TAG = "device_tools";

// Ambient context (P5): read-only device signals pushed to the agent each run via
// RunAgentInput.context. The AG-UI Context.value is a STRING, so structured signals are
// JSON-stringified into the value (see ctx_add). v1 ships local_time; battery (AXP2101) and
// device_motion (QMI8658) are the next increments. set_timer/set_alarm/show_qr tools are P7.

esp_err_t device_tools_init(void)
{
    ESP_LOGI(TAG, "init");
    return ESP_OK;
}

// Append one ambient-context entry {description, value}. `value` is always a string (the AG-UI
// Context.value type); pass a JSON-stringified object for structured signals.
static void ctx_add(cJSON *arr, const char *description, const char *value)
{
    cJSON *item = cJSON_CreateObject();
    if (!item) return;
    cJSON_AddStringToObject(item, "description", description);
    cJSON_AddStringToObject(item, "value", value);
    cJSON_AddItemToArray(arr, item);
}

// local_time → ISO-8601 with numeric offset (e.g. 2026-06-23T13:00:00-0500; UTC → +0000). The TZ
// is applied at boot from the "tz" config (POSIX TZ string, default UTC0). Only emitted once the
// clock has been set — before that it's the 1970 boot epoch, and a wrong time is worse than none.
static void add_local_time(cJSON *arr)
{
    time_t now = time(NULL);
    if (now < 1700000000) return;   // ~2023-11; below this the clock hasn't synced yet
    struct tm lt;
    localtime_r(&now, &lt);
    char iso[40];
    strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%S%z", &lt);
    ctx_add(arr, "local_time", iso);
}

cJSON *device_context_build(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    add_local_time(arr);
    // Next P5 increments append here: battery (AXP2101 pct+charging), device_motion (QMI8658).

    if (cJSON_GetArraySize(arr) == 0) {   // nothing to report yet → send no context
        cJSON_Delete(arr);
        return NULL;
    }
    return arr;
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
