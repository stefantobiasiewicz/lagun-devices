# The library as an ESP-IDF component.
#
# The very same .c files are built natively by `host/CMakeLists.txt`. There are no
# conditions on the platform or on ESP-IDF here, because this directory holds NOTHING
# that would need them — hardware begins in `ports/`.
idf_component_register(
    SRCS "src/frame.c" "src/values.c" "src/endpoint.c" "src/node.c"
         "src/key.c" "src/output.c"
    INCLUDE_DIRS "include"
)
