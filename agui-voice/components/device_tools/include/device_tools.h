// device_tools — tool registry + implementations + ambient-context provider.
//
// Ambient context per run = motion (QMI8658) + battery (AXP2101) + local_time (PCF85063).
// Builtin client tools: set_timer, set_alarm, show_qr. Dispatched from AG-UI TOOL_CALL_*.
// See docs/agui-voice-plan.md §5.3 / §6.
#pragma once

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time init: registers builtin tools.
esp_err_t device_tools_init(void);

cJSON *device_context_build(void);   // [{description,value}] ambient context
cJSON *device_tools_manifest(void);  // JSON-schema list for RunAgentInput.tools

// A tool implementation: parse args, produce a result JSON (caller owns *result).
typedef esp_err_t (*device_tool_fn)(const cJSON *args, cJSON **result);

void device_tools_register(const char *name, const cJSON *schema, device_tool_fn fn);

// Dispatch a tool call by name (returns ESP_ERR_NOT_FOUND if unknown).
esp_err_t device_tools_dispatch(const char *name, const cJSON *args, cJSON **result);

#ifdef __cplusplus
}
#endif
