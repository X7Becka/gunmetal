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
    set_tests_properties(cli_scene PROPERTIES PASS_REGULAR_EXPRESSION
        "lights: 29;spawns: 8 \\(campaign 8, deathmatch 0\\)")
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

add_executable(gm_sof_tests test_sof.cpp)
target_link_libraries(gm_sof_tests PRIVATE gm_engine)
add_test(NAME film_sof COMMAND gm_sof_tests)
gm_label(film_sof quick)

add_executable(gm_hud_tests test_hud.cpp)
target_link_libraries(gm_hud_tests PRIVATE gm_engine)
# Synthetic composition rules plus AMMO4.SOF frame 0 composited at the engine's
# HUD position; the goldens are FNV-1a64 of the whole RGB frame, produced by
# gm/sof.py + gm/bmp.py and documented in docs/HUD_CPP.md.
add_test(NAME hud COMMAND gm_hud_tests)
gm_label(hud quick)

add_executable(gm_hud_player_tests test_hud_player.cpp)
target_link_libraries(gm_hud_player_tests PRIVATE gm_engine)
# Playback clock (91 Hz ticks, .fdx count/fps, start_frame/loop/hold) on a
# synthetic 2-frame film plus AMMO4.SOF at ticks 0/91/182 and the WEAPON8 ->
# line 8 suffix binding.  Goldens and limits: docs/HUD_PLAYER.md.
add_test(NAME hud_player COMMAND gm_hud_player_tests)
gm_label(hud_player quick)

add_executable(gm_hud_bind_tests test_hud_bind.cpp)
target_link_libraries(gm_hud_bind_tests PRIVATE gm_engine)
# Screen -> film family/name/line binding plus <I x y a> placement: shipped
# .sml/.sme corpus (72 images, 15 unique), help.sme, a synthetic multi-image
# screen, state-derived family choice, missing films and malformed markup.
# Goldens and proven/hypothesis split: docs/HUD_BIND.md.
add_test(NAME hud_bind COMMAND gm_hud_bind_tests)
gm_label(hud_bind quick)

add_executable(gm_hud_compose_tests test_hud_compose.cpp)
target_link_libraries(gm_hud_compose_tests PRIVATE gm_engine)
# One-call composition of a .sml screen: binding + film + 91 Hz tick + blit.
# Shipped DESIGN/ammo4.sml reproduces hud_player's AMMO4 FNV goldens at
# (14,125) for ticks 0/91/182; synthetic fixtures pin <I> slots, clipping,
# missing EQUIP film, overflow ticks, malformed markup and short palettes.
# Pipeline and errors: docs/HUD_COMPOSE.md.
add_test(NAME hud_compose COMMAND gm_hud_compose_tests)
gm_label(hud_compose quick)

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

# Full-corpus .sbv parity: all 6843 shipped frames (500 + 4523 + 1820) hashed
# against the Python RGB reference, plus a reverse re-decode pass.  `full` only:
# the sweep takes tens of seconds and needs the game installed.
add_executable(gm_sbv_parity_tests test_sbv_parity.cpp)
target_link_libraries(gm_sbv_parity_tests PRIVATE gm_engine)
add_test(NAME sbv_parity COMMAND gm_sbv_parity_tests)
gm_label(sbv_parity full)

add_executable(gm_sml_tests test_sml.cpp)
target_link_libraries(gm_sml_tests PRIVATE gm_engine)
# Golden sweep over all 300 shipped SML/*.sml|*.sme files plus parser
# negatives; skips cleanly when the game is not installed.
add_test(NAME sml COMMAND gm_sml_tests)
gm_label(sml quick)

add_executable(gm_text_layout_tests test_text_layout.cpp)
target_link_libraries(gm_text_layout_tests PRIVATE gm_engine)
# SML text layout (docs/TEXT_LAYOUT.md): the twelve font atlases extracted from
# FRONTEND/BMP.IDX with the arguments of 0x47fe30, synthetic wrap/centre/paragraph
# cases, six real-file page goldens and the negative cases; skips cleanly when
# the game is not installed.
add_test(NAME text_layout COMMAND gm_text_layout_tests)
gm_label(text_layout quick)

add_executable(gm_sbtvm_tests test_sbtvm.cpp)
target_link_libraries(gm_sbtvm_tests PRIVATE gm_engine)
add_test(NAME sbtvm COMMAND gm_sbtvm_tests)
gm_label(sbtvm quick)

add_executable(gm_vm_tests test_vm_exec.cpp)
target_link_libraries(gm_vm_tests PRIVATE gm_engine)
# docs/VM_CPP.md: C++ port of gm/sbtvm_exec.py.  The default run checks
# level0/dm1/level1 block-by-block and all 36 main scripts; `--full` sweeps the
# whole corpus (15 061 blocks) and the 78 421 executed commands.
add_test(NAME vm COMMAND gm_vm_tests)
gm_label(vm quick)
add_test(NAME vm_full COMMAND gm_vm_tests --full)
gm_label(vm_full full)

add_executable(gm_soe_tests test_soe.cpp)
target_link_libraries(gm_soe_tests PRIVATE gm_engine)
add_test(NAME soe COMMAND gm_soe_tests)
gm_label(soe quick)

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

# Save container reader (docs/SAVE_CPP.md): container/block chain and the
# player block of block 9.  save.cpp is compiled straight into the test
# executable, so engine/CMakeLists.txt stays untouched; to use the API from
# the engine, add `src/res/save.cpp` to gm_engine there.
add_executable(gm_save_tests test_save.cpp ${CMAKE_SOURCE_DIR}/engine/src/res/save.cpp)
target_link_libraries(gm_save_tests PRIVATE gm_engine)
add_test(NAME save COMMAND gm_save_tests)
gm_label(save quick)

# Save writer (docs/SAVE_WRITER.md): byte-exact saved0/saved1 round-trip and
# typed player edits.  save.cpp is compiled into the test executable, exactly
# like gm_save_tests above, so engine/CMakeLists.txt stays untouched.
add_executable(gm_save_writer_tests test_save_writer.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/res/save.cpp)
target_link_libraries(gm_save_writer_tests PRIVATE gm_engine)
add_test(NAME save_writer COMMAND gm_save_writer_tests)
gm_label(save_writer quick)

# Typed save blocks (docs/SAVE_BLOCKS.md): switches 1, doors 2, things 3,
# sprites 6 and sound sources 7 of saved0/saved1, field edits and negatives.
add_executable(gm_save_blocks_tests test_save_blocks.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/res/save.cpp)
target_link_libraries(gm_save_blocks_tests PRIVATE gm_engine)
add_test(NAME save_blocks COMMAND gm_save_blocks_tests)
gm_label(save_blocks quick)

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
set(GM_CHECK_DEPENDS gm_tests gm_sou2_tests gm_sou1_tests gm_sof_tests gm_hud_tests gm_sbv_video_tests gm_sbtvm_tests gm_vm_tests gm_fuzz_tests gm_save_tests gunmetal)
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
