idf_component_register(
    SRCS "transport.c" "platform.c" "status.c"
    INCLUDE_DIRS "include"
    REQUIRES lagunbus driver esp_timer nvs_flash esp_hw_support
)
