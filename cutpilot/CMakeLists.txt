cmake_minimum_required(VERSION 3.24)

project(cutpilot
    VERSION 0.1.0
    DESCRIPTION "CutPilot native desktop platform"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_AUTOMOC ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/QtSetup.cmake)

add_subdirectory(libs/cutpilot-core)
add_subdirectory(libs/cutpilot-theme)
add_subdirectory(libs/cutpilot-render)
add_subdirectory(app/cutpilot-app)

option(BUILD_TESTING "Build the test targets" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
