idf_component_register(
    SRCS "device_tools.c"
    INCLUDE_DIRS "include"
    REQUIRES json)
# P5/P7 add: board_variant + esp32_s3_touch_amoled_1_8 (QMI8658 motion / AXP2101 battery
# / PCF85063 time + RTC alarm) for ambient context and the set_timer/set_alarm/show_qr tools.
