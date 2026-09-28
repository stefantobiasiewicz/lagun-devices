idf_component_register(
    SRCS "transport.c" "platform.c"
    INCLUDE_DIRS "include"
    REQUIRES lagunbus driver esp_timer nvs_flash esp_hw_support
)
