cmake_minimum_required(VERSION 3.20)

# PlatformIO owns the firmware build; CMake supplies CLion's project model.
find_program(PLATFORMIO_EXECUTABLE NAMES pio platformio
    HINTS "$ENV{HOME}/.platformio/penv/bin" REQUIRED)
set(PLATFORMIO_ENV "esp32-c5-devkitc-1" CACHE STRING "PlatformIO environment")

execute_process(
    COMMAND "${PLATFORMIO_EXECUTABLE}" run --environment "${PLATFORMIO_ENV}" --target idedata
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE PLATFORMIO_RESULT
    OUTPUT_VARIABLE PLATFORMIO_OUTPUT
    ERROR_VARIABLE PLATFORMIO_ERROR
)
if(NOT PLATFORMIO_RESULT EQUAL 0)
    message(FATAL_ERROR "PlatformIO metadata generation failed:\n${PLATFORMIO_OUTPUT}\n${PLATFORMIO_ERROR}")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/.pio/build/${PLATFORMIO_ENV}/idedata.json" PLATFORMIO_METADATA)
string(JSON PLATFORMIO_CXX GET "${PLATFORMIO_METADATA}" cxx_path)
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR riscv32)
set(CMAKE_CXX_COMPILER "${PLATFORMIO_CXX}" CACHE FILEPATH "PlatformIO C++ compiler" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(esp_display_driver LANGUAGES CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS platformio.ini)

function(platformio_json_array output)
    string(JSON count LENGTH "${PLATFORMIO_METADATA}" ${ARGN})
    set(values)
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE 0 ${last})
            string(JSON value GET "${PLATFORMIO_METADATA}" ${ARGN} ${index})
            list(APPEND values "${value}")
        endforeach()
    endif()
    set(${output} "${values}" PARENT_SCOPE)
endfunction()

# This target gives CLion real compilation settings without duplicating the
# Arduino framework build and linker setup managed by PlatformIO.
file(GLOB_RECURSE FIRMWARE_SOURCES CONFIGURE_DEPENDS src/*.cpp include/*.hpp)
add_library(esp_display_driver_code OBJECT EXCLUDE_FROM_ALL ${FIRMWARE_SOURCES})
platformio_json_array(FIRMWARE_DEFINES defines)
platformio_json_array(FIRMWARE_FLAGS cxx_flags)
platformio_json_array(FIRMWARE_INCLUDES includes build)
target_compile_definitions(esp_display_driver_code PRIVATE ${FIRMWARE_DEFINES})
target_compile_options(esp_display_driver_code PRIVATE ${FIRMWARE_FLAGS})
target_include_directories(esp_display_driver_code PRIVATE ${FIRMWARE_INCLUDES})

add_custom_target(esp_display_driver ALL
    COMMAND "${PLATFORMIO_EXECUTABLE}" run --environment "${PLATFORMIO_ENV}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
)
add_custom_target(upload
    COMMAND "${PLATFORMIO_EXECUTABLE}" run --environment "${PLATFORMIO_ENV}" --target upload
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL
)
