idf_component_register(
    SRCS "agui_client.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_http_client esp-tls esp_hw_support app_cfg)
# P2: streaming SSE over esp_http_client + esp-tls (crt bundle). RFC-6902 patch deferred to v2.
