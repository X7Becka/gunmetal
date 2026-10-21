cmake_minimum_required(VERSION 3.25)

# This checkout is developed on a machine that has no system compiler: the Zig
# toolchain in ~/tools provides `cc`/`c++` wrappers.  Pick those automatically
# when the caller did not choose a compiler and no cc/gcc/clang is on PATH, so
# the presets work out of the box there.  Every other machine is unaffected:
# -DCMAKE_C(XX)_COMPILER, CC/CXX and a real system compiler all take priority.
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED CMAKE_CXX_COMPILER
   AND NOT DEFINED ENV{CC} AND NOT DEFINED ENV{CXX})
    find_program(GM_SYSTEM_CC NAMES cc gcc clang)
    if(NOT GM_SYSTEM_CC)
        if(EXISTS "$ENV{HOME}/tools/bin/zigcc")
            set(CMAKE_C_COMPILER "$ENV{HOME}/tools/bin/zigcc" CACHE FILEPATH "C compiler")
        endif()
        if(EXISTS "$ENV{HOME}/tools/bin/zigcxx")
            set(CMAKE_CXX_COMPILER "$ENV{HOME}/tools/bin/zigcxx" CACHE FILEPATH "C++ compiler")
        endif()
    endif()
endif()

project(gunmetal LANGUAGES C CXX)

# Zig's clang silently drops ASan when the sanitizer list contains a comma
# (`-fsanitize=address,undefined`), leaving a green build with zero memory
# checks.  Refuse that spelling instead of pretending the gate works.
if(CMAKE_CXX_COMPILER MATCHES "zig" AND
   (CMAKE_CXX_FLAGS MATCHES "fsanitize=[^ ]*," OR
    CMAKE_C_FLAGS MATCHES "fsanitize=[^ ]*,"))
    message(FATAL_ERROR
        "Zig silently ignores ASan with comma-separated -fsanitize flags. "
        "Use '-fsanitize=address -fsanitize=undefined' (see the asan preset).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(GM_BUILD_TESTS "Build unit tests" ON)
option(GM_WITH_SDL3 "Use SDL3 for the platform layer when available" ON)
option(GM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# Both are opt-in: a clean build of this tree is already ~3 s (the heaviest
# translation unit, src/render/render.cpp, sets the wall clock), so neither is
# on by default.  Keep them for machines/CI where they do pay off.
option(GM_USE_PCH "Precompile the shared engine headers (gm_engine/gm_tests)" OFF)
option(GM_UNITY_BUILD "Compile gm_engine in unity batches" OFF)

add_subdirectory(engine)

if(GM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(engine/tests)
    if(GM_SANITIZE)
        # A clean exit here means ASan is not live; WILL_FAIL turns the
        # expected abort into a passing check.
        add_executable(gm_asan_selfcheck cmake/asan_selfcheck.cpp)
        add_test(NAME sanitizer_selfcheck COMMAND gm_asan_selfcheck)
        set_tests_properties(sanitizer_selfcheck PROPERTIES WILL_FAIL TRUE)
    endif()
endif()
