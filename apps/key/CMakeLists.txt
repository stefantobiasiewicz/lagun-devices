cmake_minimum_required(VERSION 3.16)

# The library comes from the `lagunbus-node` repository, checked out as a submodule in
# `lib/lagunbus-node` (zadania/FEAT-03.md, N3). A submodule rather than a path to a
# sibling directory: the ESP-IDF container mounts only this repository, so
# `../lagunbus-node` does not exist where the image is built — and a submodule pins the
# exact library commit this firmware was built against.
#
# We point at the component directory `c/lagunbus` itself, not at `c/`: next to it
# sits the POSIX port, which does not belong to ESP-IDF and has no business building
# here. The port for the board, `ports/esp32c3`, is the glue this repository keeps.
set(LAGUNBUS_NODE "${CMAKE_CURRENT_LIST_DIR}/../../lib/lagunbus-node")
if(NOT EXISTS "${LAGUNBUS_NODE}/c/lagunbus/CMakeLists.txt")
    message(FATAL_ERROR
        "lib/lagunbus-node is empty — run: git submodule update --init")
endif()
set(EXTRA_COMPONENT_DIRS
    "${LAGUNBUS_NODE}/c/lagunbus"
    "${CMAKE_CURRENT_LIST_DIR}/../../ports/esp32c3"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lagun-key)
