add_executable(gm_tests test_main.cpp)
target_link_libraries(gm_tests PRIVATE gm_engine)

if(GM_USE_PCH)
    target_precompile_headers(gm_tests PRIVATE
        <algorithm>
        <cstdint>
        <cstdio>
        <cstring>
        <optional>
        <span>
        <string>
        <string_view>
        <vector>
        ${CMAKE_SOURCE_DIR}/engine/src/core/bytes.h
        ${CMAKE_SOURCE_DIR}/engine/src/core/path.h)
endif()

# CTest labels.  Every test carries exactly one:
#   quick - cheap enough for the inner loop
#   full  - the SBV frame-hash sweep and the Python parity sweep
# Plain `ctest` still runs every test, exactly as before; the labels only drive
# `ctest -L quick`/`-L full` and the presets in CMakePresets.json.
function(gm_label test_name label)
    set_tests_properties(${test_name} PROPERTIES LABELS ${label})
endfunction()

add_test(NAME core COMMAND gm_tests)
gm_label(core quick)

add_test(NAME core_full COMMAND gm_tests --full)
gm_label(core_full full)

add_test(NAME cli_version COMMAND gunmetal --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "gunmetal 0.1.0")
gm_label(cli_version quick)

add_test(NAME cli_headless COMMAND gunmetal --headless --frames 2)
set_tests_properties(cli_headless PROPERTIES PASS_REGULAR_EXPRESSION "frames=2")
gm_label(cli_headless quick)

if(EXISTS "/mnt/d/games/Gunmetal 3dfx/level0.sbt")
    add_test(NAME cli_scene COMMAND gunmetal --scene level0)
    set_tests_properties(cli_scene PROPERTIES PASS_REGULAR_EXPRESSION "lights: 29")
    gm_label(cli_scene quick)
endif()

if(EXISTS "/mnt/d/games/Gunmetal 3dfx/MUSIC/Dm1A.sog")
    add_test(NAME cli_song COMMAND gunmetal --song Dm1A)
    set_tests_properties(cli_song PROPERTIES PASS_REGULAR_EXPRESSION "tracks: 8")
    gm_label(cli_song quick)
endif()

if(EXISTS "/mnt/d/games/Gunmetal 3dfx/Vid/Intro.sbv")
    add_test(NAME cli_play COMMAND gunmetal --headless --play Intro --frames 3)
    set_tests_properties(cli_play PROPERTIES PASS_REGULAR_EXPRESSION "frames=3")
    gm_label(cli_play quick)
endif()

if(EXISTS "/mnt/d/games/Gunmetal 3dfx/level0.sbw")
    add_test(NAME cli_level_frames COMMAND gunmetal --headless --level level0 --frames 2)
    set_tests_properties(cli_level_frames PROPERTIES PASS_REGULAR_EXPRESSION "frames=2")
    gm_label(cli_level_frames quick)
endif()

add_test(NAME cli_dump COMMAND gunmetal --dump ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)
set_tests_properties(cli_dump PROPERTIES PASS_REGULAR_EXPRESSION "size: [0-9]+ bytes")
gm_label(cli_dump quick)

add_test(NAME cli_dump_missing COMMAND gunmetal --dump ${CMAKE_CURRENT_SOURCE_DIR}/missing.bin)
set_tests_properties(cli_dump_missing PROPERTIES WILL_FAIL TRUE)
gm_label(cli_dump_missing quick)

add_executable(gm_sou2_tests test_sou2.cpp)
target_link_libraries(gm_sou2_tests PRIVATE gm_engine)
add_test(NAME sou2 COMMAND gm_sou2_tests)
gm_label(sou2 quick)
add_test(NAME sou2_full COMMAND gm_sou2_tests --full)
gm_label(sou2_full full)

add_executable(gm_sou1_tests test_sou1.cpp)
target_link_libraries(gm_sou1_tests PRIVATE gm_engine)
# `sou1` pins 11 records covering every sample rate, mono/stereo and the
# long-track boundaries; `sou1_full` checks all 983 SOU1 entries against
# docs/manifests/sou1-fnv.json (both parse all 999 container headers).
add_test(NAME sou1 COMMAND gm_sou1_tests)
gm_label(sou1 quick)
add_test(NAME sou1_full COMMAND gm_sou1_tests --full)
gm_label(sou1_full full)

add_executable(gm_sbv_video_tests test_sbv_video.cpp)
target_link_libraries(gm_sbv_video_tests PRIVATE gm_engine)
# `ctest` runs the full 500-frame Intro sweep; `gm_sbv_video_tests --quick`
# checks every 25th golden frame instead (same goldens, same hashes).
add_test(NAME sbv_video COMMAND gm_sbv_video_tests)
gm_label(sbv_video full)

add_executable(gm_sbtvm_tests test_sbtvm.cpp)
target_link_libraries(gm_sbtvm_tests PRIVATE gm_engine)
add_test(NAME sbtvm COMMAND gm_sbtvm_tests)
gm_label(sbtvm quick)

add_executable(gm_fuzz_tests test_fuzz.cpp)
target_link_libraries(gm_fuzz_tests PRIVATE gm_engine)
# Quick by default; `gm_fuzz_tests --full` widens the mutation budgets.
# The TIMEOUT is the watchdog for an in-process infinite loop (the harness can
# only measure a case after the call returns).
add_test(NAME fuzz COMMAND gm_fuzz_tests --quick)
gm_label(fuzz quick)
set_tests_properties(fuzz PROPERTIES TIMEOUT 300)

add_test(NAME fuzz_full COMMAND gm_fuzz_tests --full)
gm_label(fuzz_full full)
set_tests_properties(fuzz_full PROPERTIES TIMEOUT 900)

find_program(GM_PYTHON3 python3)
if(GM_PYTHON3 AND EXISTS "/mnt/d/games/Gunmetal 3dfx")
    add_test(NAME parity_levels
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/parity_levels.py
                     --gunmetal $<TARGET_FILE:gunmetal>)
    gm_label(parity_levels full)
endif()

# Fast targets for the inner loop: build the binaries, then run CTest.
#   cmake --build build/dev --target check-quick
#   cmake --build build/test --target check
set(GM_CHECK_DEPENDS gm_tests gm_sou2_tests gm_sou1_tests gm_sbv_video_tests gm_sbtvm_tests gm_fuzz_tests gunmetal)
add_custom_target(check-quick
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -L quick -j 8
            --output-on-failure
    DEPENDS ${GM_CHECK_DEPENDS}
    USES_TERMINAL
    COMMENT "CTest: quick label")
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} -j 8 --output-on-failure
    DEPENDS ${GM_CHECK_DEPENDS}
    USES_TERMINAL
    COMMENT "CTest: all tests")
