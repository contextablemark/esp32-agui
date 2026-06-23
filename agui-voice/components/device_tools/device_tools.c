#include "device_tools.h"

#include <stdio.h>
#include <stdbool.h>
#include <time.h>

#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

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
    ctx_add(arr, "current_local_time", iso);   // clearer key so the agent prefers it over searching
}

// --- AXP2101 PMIC @ 0x34 on the shared BSP I2C bus (battery + PWR key) ------------------------
// Register-level reads mirroring XPowersLib (so no Arduino-C++ lib is vendored). We only do
// MEASUREMENT-enable + PWRKEY-IRQ-enable writes (battery ADC on, TS-pin off; latch PWRKEY presses
// so we can poll them) — exactly the read path the proven 01_AXP2101 example uses — and never
// touch power rails or charger settings.
#define AXP2101_ADDR        0x34
#define AXP_REG_STATUS1     0x00   // bit3 = battery connected
#define AXP_REG_STATUS2     0x01   // (val >> 5) == 1 => charging
#define AXP_REG_ADC_CTRL    0x30   // bit0 = batt-voltage ADC enable; bit1 = TS-pin measure
#define AXP_REG_IRQ_EN2     0x41   // INTEN2: enable PWRKEY short=bit3 / long=bit2 IRQ latching
#define AXP_REG_IRQ_STS2    0x49   // INTSTS2: PWRKEY short=bit3 / long=bit2 status (write-1-to-clear)
#define AXP_REG_BAT_PERCENT 0xA4   // 0..100
#define AXP_IRQ_PKEY_SHORT  (1u << 3)
#define AXP_IRQ_PKEY_LONG   (1u << 2)

static i2c_master_dev_handle_t s_axp;   // lazily added to the BSP bus

static esp_err_t axp_rd(uint8_t reg, uint8_t *val)
{ return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 100); }
static esp_err_t axp_wr(uint8_t reg, uint8_t val)
{ uint8_t b[2] = { reg, val }; return i2c_master_transmit(s_axp, b, 2, 100); }

// Bring the AXP2101 up on the shared BSP I2C bus + enable battery measurement. Lazy & idempotent
// (bsp_i2c_init() is a no-op if already initialized), so call order vs the display doesn't matter.
static bool axp_ensure(void)
{
    if (s_axp) return true;
    if (bsp_i2c_init() != ESP_OK) return false;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return false;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                .device_address = AXP2101_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) { s_axp = NULL; return false; }
    uint8_t adc;   // measurement-only: enable batt ADC (needed for the % gauge), disable TS pin
    if (axp_rd(AXP_REG_ADC_CTRL, &adc) == ESP_OK) {
        adc = (uint8_t)((adc | (1u << 0)) & ~(1u << 1));
        axp_wr(AXP_REG_ADC_CTRL, adc);
    }
    uint8_t irqen; // latch PWRKEY short/long-press IRQs so device_power_key_short_press() can poll them
    if (axp_rd(AXP_REG_IRQ_EN2, &irqen) == ESP_OK)
        axp_wr(AXP_REG_IRQ_EN2, (uint8_t)(irqen | AXP_IRQ_PKEY_SHORT | AXP_IRQ_PKEY_LONG));
    return true;
}

// battery → {"pct":<0-100>,"charging":<bool>} (JSON-stringified). Skipped if no battery is present.
static void add_battery(cJSON *arr)
{
    if (!axp_ensure()) return;
    uint8_t s1, s2, pct;
    if (axp_rd(AXP_REG_STATUS1, &s1) != ESP_OK || !(s1 & (1u << 3))) return;   // no battery connected
    if (axp_rd(AXP_REG_BAT_PERCENT, &pct) != ESP_OK || pct > 100) return;      // invalid gauge read
    bool charging = (axp_rd(AXP_REG_STATUS2, &s2) == ESP_OK) && ((s2 >> 5) == 0x01);
    char val[48];
    snprintf(val, sizeof val, "{\"pct\":%u,\"charging\":%s}", pct, charging ? "true" : "false");
    ctx_add(arr, "battery", val);
}

// PWR button (AXP2101 PWRKEY) short-press since the last call. The PMIC latches short/long-press
// events in INTSTS2 (we poll it — the AXP IRQ pin isn't wired to a GPIO on this board). A long press
// is a hardware power-off the PMIC does on its own, so only short presses are reported (used by the
// chat_ui screen-power saver to wake the display). Both bits are cleared (write-1-to-clear).
bool device_power_key_short_press(void)
{
    if (!axp_ensure()) return false;
    uint8_t sts;
    if (axp_rd(AXP_REG_IRQ_STS2, &sts) != ESP_OK) return false;
    uint8_t hit = sts & (AXP_IRQ_PKEY_SHORT | AXP_IRQ_PKEY_LONG);
    if (hit) axp_wr(AXP_REG_IRQ_STS2, hit);          // clear only the PWRKEY bits we consumed
    return (sts & AXP_IRQ_PKEY_SHORT) != 0;
}

cJSON *device_context_build(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    add_local_time(arr);
    add_battery(arr);
    // Next P5 increment appends here: device_motion (QMI8658).

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
