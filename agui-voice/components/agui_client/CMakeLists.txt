idf_component_register(
    SRCS "agui_client.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_http_client)
# P2 adds streaming SSE over esp_http_client + esp-tls; RFC-6902 patch deferred to v2.
