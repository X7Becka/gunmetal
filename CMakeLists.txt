cmake_minimum_required(VERSION 3.25)
project(gunmetal LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GM_BUILD_TESTS "Build unit tests" ON)
option(GM_WITH_SDL3 "Use SDL3 for the platform layer when available" ON)
option(GM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_subdirectory(engine)

if(GM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(engine/tests)
endif()
