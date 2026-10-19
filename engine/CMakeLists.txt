add_library(gm_engine STATIC
    src/core/bytes.cpp
    src/core/log.cpp
    src/core/path.cpp
    src/core/config.cpp
    src/core/time.cpp
    src/platform/window.cpp
    src/res/anim.cpp
    src/res/level.cpp
    src/res/sbv.cpp
    src/res/cache.cpp
    src/res/bigfile.cpp
    src/res/bmp.cpp
    src/res/png.cpp
    src/res/sbm.cpp
    src/res/sbw.cpp
    src/res/sbc.cpp
    src/res/sob.cpp
    src/res/sbt.cpp
    src/res/sou.cpp
    src/res/text.cpp
)

target_include_directories(gm_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(gm_engine PUBLIC cxx_std_20)

if(GM_WARNINGS_AS_ERRORS)
    set(GM_WARN_FLAGS -Werror)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(gm_engine PRIVATE -Wall -Wextra -Wpedantic ${GM_WARN_FLAGS})
endif()

if(GM_WITH_SDL3)
    find_package(SDL3 CONFIG QUIET)
endif()

if(GM_WITH_SDL3 AND SDL3_FOUND)
    target_compile_definitions(gm_engine PUBLIC GM_HAVE_SDL3=1)
    target_link_libraries(gm_engine PUBLIC SDL3::SDL3)
    message(STATUS "platform: SDL3 ${SDL3_VERSION}")
else()
    target_compile_definitions(gm_engine PUBLIC GM_HAVE_SDL3=0)
    message(STATUS "platform: headless only (SDL3 not found; set SDL3_DIR or -DGM_WITH_SDL3=OFF)")
endif()

add_executable(gunmetal src/main.cpp)
target_link_libraries(gunmetal PRIVATE gm_engine)

install(TARGETS gunmetal RUNTIME DESTINATION bin)
