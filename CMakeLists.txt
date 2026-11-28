# Based on the supplied cmake_starter: root settings plus hal/ and app/ builds.
cmake_minimum_required(VERSION 3.18)
project(board_diagnostics VERSION 1.0 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(ENABLE_HARDWARE "Build Linux SPI and libgpiod hardware support" ON)
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
add_compile_definitions(_POSIX_C_SOURCE=200809L)
add_compile_options(-Wall -Werror -Wpedantic -Wextra -fdiagnostics-color)
find_package(Threads REQUIRED)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
add_subdirectory(hal)
add_subdirectory(app)
add_subdirectory(controller)
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME diagnostic_scenarios
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.py"
                "$<TARGET_FILE:board_diagnostics>")
    add_test(NAME controller_scenarios
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/controller/tests/integration.py"
                "$<TARGET_FILE:circuit_controller>"
                "${CMAKE_CURRENT_SOURCE_DIR}/controller/tools/mock_board.py")
    set_tests_properties(controller_scenarios PROPERTIES TIMEOUT 90)
endif()
