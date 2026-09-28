# Native build — the library and everything that can be built from it WITHOUT ESP-IDF.
#
# EVERYTHING that does not need ESP-IDF is built here: the library, the unit tests and
# `lagun-node` — a node over SocketCAN, the same code that goes onto a board.
#
# The key and output logic lives in the library rather than in the ESP32 applications
# precisely so that it can be checked here. What is left on the board is reading a pin
# and driving a relay — the rest is tested before it is flashed.
#
# The board build (`ports/esp32c3` and `idf.py`) has nothing to do with this file at
# all. Same .c files, a different build.
cmake_minimum_required(VERSION 3.16)
project(lagunbus_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wconversion)

add_library(lagunbus
    ../components/lagunbus/src/frame.c
    ../components/lagunbus/src/values.c
    ../components/lagunbus/src/endpoint.c
    ../components/lagunbus/src/node.c
    ../components/lagunbus/src/key.c
    ../components/lagunbus/src/output.c
)
target_include_directories(lagunbus PUBLIC ../components/lagunbus/include)

# The POSIX port. It builds EVERYWHERE — on a machine without SocketCAN what is left
# of it is a polite refusal instead of a socket. The same trick as on the hub side.
add_library(lagunbus_posix
    ../ports/posix/transport.c
    ../ports/posix/platform.c
)
target_include_directories(lagunbus_posix PUBLIC ../ports/posix/include)
target_link_libraries(lagunbus_posix PUBLIC lagunbus)

enable_testing()

foreach(name frame values interfaces node key output)
    add_executable(test_${name} test_${name}.c)
    target_link_libraries(test_${name} lagunbus_posix)
    target_include_directories(test_${name} PRIVATE .)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# `lagun-node` — the thing that can actually be run. On macOS it builds and politely
# refuses; on a Raspberry Pi it opens the bus.
add_executable(lagun-node lagun_node.c)
target_link_libraries(lagun-node lagunbus_posix)

# The registry has to agree with the specification — same as on the hub side.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME registry
        COMMAND ${Python3_EXECUTABLE} ../tools/generate_registry.py --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()
