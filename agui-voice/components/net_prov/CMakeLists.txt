idf_component_register(
    SRCS "net_prov.c" "portal.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash esp_http_server lwip app_cfg)
