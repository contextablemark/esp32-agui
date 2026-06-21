// net_prov captive portal (P0.5) — SoftAP "AMOLED-setup" + HTTP setup form + DNS
// catch-all so phones auto-open the page. User submits SSID/pass → saved to NVS.

#include "net_prov.h"
#include "app_cfg.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "net_portal";

#define PBIT_SAVED BIT0
#define PORTAL_IP  "192.168.4.1"

static httpd_handle_t     s_httpd;
static esp_netif_t       *s_ap_netif;
static EventGroupHandle_t s_portal_eg;
static TaskHandle_t       s_dns_task;
static volatile bool      s_dns_run;

static const char FORM[] =
    "<!doctype html><html><head><meta name=viewport "
    "content='width=device-width,initial-scale=1'><title>AMOLED setup</title>"
    "<style>body{font-family:sans-serif;margin:2em;max-width:30em}"
    "input{display:block;width:100%;padding:.6em;margin:.4em 0;font-size:1em}"
    "button{padding:.7em 1.2em;font-size:1em}</style></head><body>"
    "<h2>AG-UI device setup</h2>"
    "<form method=POST action=/save>"
    "<label>WiFi network (SSID)</label><input name=ssid autofocus>"
    "<label>WiFi password</label><input name=pass type=password>"
    "<label>Soniox API key</label><input name=soniox>"
    "<label>AG-UI endpoint URL</label><input name=agui_url>"
    "<label>AG-UI bearer token (optional)</label><input name=agui_token>"
    "<p style='color:#666;font-size:.85em'>Leave WiFi blank if already connected; "
    "fill the Soniox key to enable voice and the AG-UI URL to enable the agent.</p>"
    "<button type=submit>Save &amp; connect</button></form></body></html>";

// ---- tiny form helpers ---------------------------------------------------

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// URL-decode src into dst (handles %xx and '+').
static void url_decode(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dstsz; i++) {
        if (src[i] == '+') {
            dst[o++] = ' ';
        } else if (src[i] == '%' && hexv(src[i + 1]) >= 0 && hexv(src[i + 2]) >= 0) {
            dst[o++] = (char)(hexv(src[i + 1]) * 16 + hexv(src[i + 2]));
            i += 2;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
}

// Extract field `name` from urlencoded body into out (decoded). Returns true if found.
static bool form_field(const char *body, const char *name, char *out, size_t outsz)
{
    char key[24];
    int kl = snprintf(key, sizeof(key), "%s=", name);
    const char *p = body;
    while ((p = strstr(p, key))) {
        if (p == body || p[-1] == '&') {     // key at start of a pair
            p += kl;
            const char *end = strchr(p, '&');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            char raw[256];
            if (len >= sizeof(raw)) len = sizeof(raw) - 1;
            memcpy(raw, p, len);
            raw[len] = '\0';
            url_decode(raw, out, outsz);
            return true;
        }
        p += kl;
    }
    return false;
}

// ---- HTTP handlers -------------------------------------------------------

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, FORM, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[1280];   // holds all urlencoded fields (ssid/pass/soniox/agui_url/agui_token)
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = '\0';

    char ssid[33] = {0}, pass[65] = {0}, soniox[APP_CFG_VAL_MAX] = {0};
    char agui_url[APP_CFG_VAL_MAX] = {0}, agui_token[APP_CFG_VAL_MAX] = {0};
    bool have_ssid   = form_field(body, "ssid", ssid, sizeof(ssid)) && ssid[0];
    bool have_soniox = form_field(body, "soniox", soniox, sizeof(soniox)) && soniox[0];
    bool have_url    = form_field(body, "agui_url", agui_url, sizeof(agui_url)) && agui_url[0];
    bool have_token  = form_field(body, "agui_token", agui_token, sizeof(agui_token)) && agui_token[0];
    if (!have_ssid && !have_soniox && !have_url && !have_token) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nothing to save");
        return ESP_FAIL;
    }
    if (have_ssid) {
        form_field(body, "pass", pass, sizeof(pass));
        net_creds_add(ssid, pass);
    }
    if (have_soniox) app_cfg_set(APP_CFG_SONIOX_KEY, soniox);
    if (have_url)    app_cfg_set(APP_CFG_AGUI_URL, agui_url);
    if (have_token)  app_cfg_set(APP_CFG_AGUI_TOKEN, agui_token);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;margin:2em'>"
        "<h3>Saved.</h3><p>The device is applying settings now. You can close this page.</p>"
        "</body></html>");
    xEventGroupSetBits(s_portal_eg, PBIT_SAVED);
    return ESP_OK;
}

// Catch-all: redirect every other request to the portal (triggers OS captive detection).
static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

// ---- minimal DNS catch-all (answers every A query with the portal IP) ----

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(sock); vTaskDelete(NULL); return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &cl);
        if (n < (int)sizeof(uint16_t) * 6) continue;   // not a sane DNS query
        // Build a response in place: flags = standard response, 1 answer.
        buf[2] = 0x81; buf[3] = 0x80;          // QR=1, RD=1, RA=1
        buf[6] = 0x00; buf[7] = 0x01;          // ANCOUNT = 1
        if (n + 16 > (int)sizeof(buf)) continue;
        uint8_t *a = buf + n;
        *a++ = 0xC0; *a++ = 0x0C;              // name pointer to question
        *a++ = 0x00; *a++ = 0x01;              // type A
        *a++ = 0x00; *a++ = 0x01;              // class IN
        *a++ = 0x00; *a++ = 0x00; *a++ = 0x00; *a++ = 0x3C;  // TTL 60
        *a++ = 0x00; *a++ = 0x04;              // RDLENGTH 4
        *a++ = 192; *a++ = 168; *a++ = 4; *a++ = 1;          // 192.168.4.1
        sendto(sock, buf, a - buf, 0, (struct sockaddr *)&cli, cl);
    }
    close(sock);
    vTaskDelete(NULL);
}

// ---- public API ----------------------------------------------------------

esp_err_t net_portal_start(const char *ap_ssid)
{
    if (!ap_ssid) ap_ssid = "AMOLED-setup";
    if (!s_portal_eg) s_portal_eg = xEventGroupCreate();
    xEventGroupClearBits(s_portal_eg, PBIT_SAVED);

    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) return ESP_FAIL;
    httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t u_save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t u_any  = { .uri = "/*", .method = HTTP_GET, .handler = redirect_get };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_save);
    httpd_register_uri_handler(s_httpd, &u_any);

    s_dns_run = true;
    xTaskCreate(dns_task, "portal_dns", 3072, NULL, 5, &s_dns_task);

    ESP_LOGI(TAG, "captive portal up: SSID '%s' → http://%s/", ap_ssid, PORTAL_IP);
    return ESP_OK;
}

bool net_portal_wait_saved(uint32_t timeout_ms)
{
    if (!s_portal_eg) return false;
    TickType_t to = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t b = xEventGroupWaitBits(s_portal_eg, PBIT_SAVED, pdTRUE, pdFALSE, to);
    return (b & PBIT_SAVED) != 0;
}

void net_portal_stop(void)
{
    s_dns_run = false;
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    // Give the DNS task a tick to exit its 1s-timeout recv and self-delete.
    vTaskDelay(pdMS_TO_TICKS(1100));
    s_dns_task = NULL;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "captive portal down");
}
