idf_component_register(
    SRCS "agui_voice_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        net_prov
        app_cfg
        soniox_client
        agui_client
        device_tools
        chat_ui
        board_variant
        esp32_s3_touch_amoled_1_8
        espressif__button)
