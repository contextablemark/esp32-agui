idf_component_register(
    SRCS "soniox_client.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common)
# P1 adds: esp_websocket_client (managed), esp-tls, esp_codec_dev (mic capture).
