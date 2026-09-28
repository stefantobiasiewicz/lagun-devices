cmake_minimum_required(VERSION 3.16)

# The library and the port live one level up and are SHARED by both applications.
# Copying them into each project would end in two versions of the protocol.
#
# We point at `ports/esp32c3` directly rather than at all of `ports/`: next to it sits
# `ports/posix`, which does not belong to ESP-IDF and has no business building here.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
    "${CMAKE_CURRENT_LIST_DIR}/../../ports/esp32c3"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lagun-key)
