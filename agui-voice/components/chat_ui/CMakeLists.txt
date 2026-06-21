idf_component_register(
    SRCS "chat_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES json lvgl esp32_s3_touch_amoled_1_8)
# P3 adds the LVGL chat list/bubbles; P4 status; P6 interrupt prompt + lv_qrcode.
