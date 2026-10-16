# The `esp32c3` component is our port — ESP-IDF names components after the directory,
# so `ports/esp32c3/` yields `esp32c3`. This is not the chip target name.
idf_component_register(
    SRCS "main.c"
    REQUIRES lagunbus esp32c3 driver ws2812
)
