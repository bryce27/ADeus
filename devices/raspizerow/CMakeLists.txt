cmake_minimum_required(VERSION 3.10)
project(adeus_raspizerow)

# Options
option(ENABLE_BLUETOOTH "Enable Bluetooth LE support" OFF)

# Include FetchContent module
include(FetchContent)

# Fetch cxxopts
FetchContent_Declare(
    cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG v3.2.0
)
FetchContent_MakeAvailable(cxxopts)

# Source files
set(SOURCES
    main.cpp
)

# Add Bluetooth sources if enabled
if(ENABLE_BLUETOOTH)
    list(APPEND SOURCES
        bluetooth/ble_server.cpp
    )
    add_definitions(-DENABLE_BLUETOOTH)
    message(STATUS "Bluetooth LE support enabled")
endif()

# Add executable target
add_executable(main ${SOURCES})

# Set C++ standard
set_target_properties(main PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Find required libraries
find_package(ALSA REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Base libraries
target_link_libraries(main PRIVATE
    ${ALSA_LIBRARIES}
    ${CURL_LIBRARIES}
    Threads::Threads
    cxxopts
)

# Add Bluetooth library if enabled
if(ENABLE_BLUETOOTH)
    find_library(BLUETOOTH_LIB bluetooth)
    if(BLUETOOTH_LIB)
        target_link_libraries(main PRIVATE ${BLUETOOTH_LIB})
    else()
        message(FATAL_ERROR "Bluetooth library not found. Install with: sudo apt-get install libbluetooth-dev")
    endif()
endif()

# Include directories
target_include_directories(main PRIVATE
    ${ALSA_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
