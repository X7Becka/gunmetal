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

    # LIGHT chassis (TICK.sob) at a proven-open level0 hangar bay, camera
    # behind it.  The spawn alcove itself is too tight for a 140-unit boom,
    # hence the --third-pos photo override.
    add_test(NAME cli_third_person COMMAND gunmetal --level level0 --third-person
        --third-pos 0,-550,11.5 --third-yaw 90 --third-distance 140
        --third-height 45 --fov 60 --render
        ${CMAKE_CURRENT_BINARY_DIR}/third_person.png --width 320 --height 200)
    set_tests_properties(cli_third_person PROPERTIES PASS_REGULAR_EXPRESSION
        "third-person: model=TICK;output: .*third_person\\.png")
    gm_label(cli_third_person quick)
endif()

add_test(NAME cli_dump COMMAND gunmetal --dump ${CMAKE_CURRENT_SOURCE_DIR}/test_main.cpp)
set_tests_properties(cli_dump PROPERTIES PASS_REGULAR_EXPRESSION "size: [0-9]+ bytes")
gm_label(cli_dump quick)

add_test(NAME cli_dump_missing COMMAND gunmetal --dump ${CMAKE_CURRENT_SOURCE_DIR}/missing.bin)
set_tests_properties(cli_dump_missing PROPERTIES WILL_FAIL TRUE)
gm_label(cli_dump_missing quick)

add_executable(gm_collide_tests test_collide.cpp)
target_link_libraries(gm_collide_tests PRIVATE gm_engine)
# Ray/sphere casts over .sbw triangles (docs/COLLIDE.md): cube, floor and
# two standalone triangles for exact t/point/normal, misses, parallel rays,
# origins inside, radius/edge/vertex contacts, degenerate polygons, NaN/inf,
# plus level0 floor/wall/AABB goldens from the Python sweep.
add_test(NAME collide COMMAND gm_collide_tests)
gm_label(collide quick)

add_executable(gm_player_controller_tests test_player_controller.cpp)
target_link_libraries(gm_player_controller_tests PRIVATE gm_engine)
# Deterministic 91 Hz player controller (docs/PLAYER_CONTROLLER.md): synthetic
# floor/wall/step/ceiling worlds with exact post-tick positions, falling,
# jumping, sliding, step-up/down, speed clamp, two-run determinism, negatives,
# plus level0 spawn and a 100-tick forward golden.
add_test(NAME player_controller COMMAND gm_player_controller_tests)
gm_label(player_controller quick)

add_executable(gm_camera_tests test_camera.cpp)
target_link_libraries(gm_camera_tests PRIVATE gm_engine)
# First/third-person camera rig (docs/CAMERA_CPP.md): the proven 0x41e025
# basis, eye = pos.z + player[0x2F50], ±π/6 / ±20° / ±π/2 clamps, the
# SET_CAMERA_*_SHAKE triangle wave and its exact duration table, the dormant
# view-pitch decay, SOURCEPLAYER_RECOIL, focal 1 / 5/6 (320x200), near/far,
# third-person occlusion, smoothing, two-run determinism and negatives; real
# dm1/level0/level1 campaign spawns against gm.scene.first_person goldens.
add_test(NAME camera_rig COMMAND gm_camera_tests)
gm_label(camera_rig quick)

add_executable(gm_controls_tests test_controls.cpp)
target_link_libraries(gm_controls_tests PRIVATE gm_engine)
# `.key` profiles and controller config (docs/CONTROLS_CPP.md): synthetic
# bind/override/conflict, pressed/held/released edges, axis deadzone + invert,
# diagonal length 1, unknown-key preservation and truncated/bad-magic
# negatives; real PERFECT/SPLIT1/SPLIT2 (29/19/19 slots, 45 pairs, identical
# stack tail) and GUNMETAL.CFG (profiles, axes 4/8/0, 2200 ms, buttons
# 5/15/5/15, four identical controller records).
add_test(NAME controls COMMAND gm_controls_tests)
gm_label(controls quick)

add_executable(gm_sou2_tests test_sou2.cpp)
target_link_libraries(gm_sou2_tests PRIVATE gm_engine)
add_test(NAME sou2 COMMAND gm_sou2_tests)
gm_label(sou2 quick)
add_test(NAME sou2_full COMMAND gm_sou2_tests --full)
gm_label(sou2_full full)

# Doors/switches/lifts state machine (docs/DOORS_CPP.md).  Quick run pins
# dm1.sbt+dm1.sbw (61 doors, 129 switches); `--full` sweeps all 36 levels and
# the corpus totals 1779/3964/45 with 1779/1779 object bindings.
add_executable(gm_doors_tests test_doors.cpp)
target_link_libraries(gm_doors_tests PRIVATE gm_engine)
add_test(NAME doors COMMAND gm_doors_tests)
gm_label(doors quick)
add_test(NAME doors_full COMMAND gm_doors_tests --full)
gm_label(doors_full full)

# Thing runtime (docs/THINGS_CPP.md).  Quick run pins synthetic SBT/SCRIPT
# cases and dm1/level0; `--full` sweeps all 36 levels and the corpus totals
# 4008/7563/7617 with 391 starts and 2896 valid thing-type model refs.
add_executable(gm_things_tests test_things.cpp)
target_link_libraries(gm_things_tests PRIVATE gm_engine)
add_test(NAME things COMMAND gm_things_tests)
gm_label(things quick)
add_test(NAME things_full COMMAND gm_things_tests --full)
gm_label(things_full full)

# Minimal thing AI (docs/AI_CPP.md).  Synthetic state transitions, LOS through
# render/collide, timers, leash/return, death and dm1/level0 attach goldens.
add_executable(gm_ai_tests test_ai.cpp)
target_link_libraries(gm_ai_tests PRIVATE gm_engine)
add_test(NAME thing_ai COMMAND gm_ai_tests)
gm_label(thing_ai quick)

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

# Indexed .sbm sprite blitter (docs/SPRITE.md): nearest/bilinear, wrap, clip,
# flips, integer scale, opacity, additive and real archive sprites.
add_executable(gm_sprite_tests test_sprite.cpp)
target_link_libraries(gm_sprite_tests PRIVATE gm_engine)
add_test(NAME sprite COMMAND gm_sprite_tests)
gm_label(sprite quick)

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

# `audio/sou1.h` (docs/SOU1_CPP.md): source-rate `Sou1Sound`, a chunked
# `Sou1Stream` and the container checks on top of the same verified decoder.
# `sou1_audio` pins synthetic vectors and seven real records; `sou1_audio_full`
# hashes all 983 entries forward and in reverse index order.
add_executable(gm_sou1_audio_tests test_sou1_audio.cpp)
target_link_libraries(gm_sou1_audio_tests PRIVATE gm_engine)
add_test(NAME sou1_audio COMMAND gm_sou1_audio_tests)
gm_label(sou1_audio quick)
add_test(NAME sou1_audio_full COMMAND gm_sou1_audio_tests --full)
gm_label(sou1_audio_full full)

add_executable(gm_mixer_tests test_mixer.cpp)
target_link_libraries(gm_mixer_tests PRIVATE gm_engine)
# Deterministic 32-voice software mixer (docs/AUDIO_MIXER.md): synthetic
# impulse/ramp/DC fixtures for pan, loop, stop, resample, distance, the pool
# limit, clipping and negative parameters; bit-identical reruns; and two real
# SOU1 records (8 kHz mono, 32 kHz stereo) hashed over the first 4096 stereo
# frames at 22050 Hz.
add_test(NAME mixer COMMAND gm_mixer_tests)
gm_label(mixer quick)

# Platform audio output (docs/AUDIO_OUTPUT.md): deterministic null backend for
# the ring, underrun/overrun accounting, pause, the 0..255 volume curve and the
# PullSource hook (including a real Mixer); the SDL3 path runs only when a
# driver/device exists and prints an explicit SKIP reason otherwise.
add_executable(gm_audio_output_tests test_audio_output.cpp)
target_link_libraries(gm_audio_output_tests PRIVATE gm_engine)
add_test(NAME audio_output COMMAND gm_audio_output_tests)
gm_label(audio_output quick)

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

add_executable(gm_soo_player_tests test_soo_player.cpp)
target_link_libraries(gm_soo_player_tests PRIVATE gm_engine)
# Runtime .soo player (docs/SOO_PLAYER.md): synthetic hold/loop timelines,
# vertex/normal morph, SOB name cache, clean failures, and four golden moments
# of Cultist.soo, Cloud.soo and kingbody.soo (vertex/normal FNV-1a64).
add_test(NAME soo_player COMMAND gm_soo_player_tests)
gm_label(soo_player quick)

add_executable(gm_screen_tests test_screen.cpp)
target_link_libraries(gm_screen_tests PRIVATE gm_engine)
# One SML/SME screen runtime (docs/SCREEN_CPP.md): parse via res/sml, layout via
# render/text_layout, <I> via render/sprite, pages/lines/selection/scroll.  The
# synthetic cases cover every exposed tag; help/level21/weapon20/level10 pin
# page/line counts and RGB FNV, <I> texels are checked against res/bmp.  Skips
# cleanly when the game is not installed.
add_test(NAME screen COMMAND gm_screen_tests)
gm_label(screen quick)

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

# Game state bridge (docs/GAME_STATE.md): save block 9/2 -> PlayerState /
# WorldState, write-back with unknown blocks preserved, shipped saves,
# byte-exact no-op and negatives.  state.cpp is part of gm_engine.
add_executable(gm_events_tests test_events.cpp)
target_link_libraries(gm_events_tests PRIVATE gm_engine)
add_test(NAME events COMMAND gm_events_tests)
gm_label(events quick)

add_executable(gm_game_state_tests test_game_state.cpp)
target_link_libraries(gm_game_state_tests PRIVATE gm_engine)
add_test(NAME game_state COMMAND gm_game_state_tests)
gm_label(game_state quick)

# Weapon runtime (docs/WEAPONS_CPP.md): DEFINE_WEAPONS/DEFINE_AMMO, save
# ammo/mask/slot, fixed-tick cooldown/reload, hitscan/projectile, spread,
# recoil and damage.  Synthetic tables/worlds plus saved0/saved1 + level0.sbt
# first-shot goldens.
add_executable(gm_weapons_tests test_weapons.cpp)
target_link_libraries(gm_weapons_tests PRIVATE gm_engine)
add_test(NAME weapons COMMAND gm_weapons_tests)
gm_label(weapons quick)

# Player input -> weapon tick seam (docs/WEAPONS_CPP.md §3.6): trigger
# hold/edge, slot request, charge threshold and the runtime spawn/event bridge.
add_executable(gm_player_trigger_tests test_player_trigger.cpp)
target_link_libraries(gm_player_trigger_tests PRIVATE gm_engine)
add_test(NAME player_trigger COMMAND gm_player_trigger_tests)
gm_label(player_trigger quick)

# The original's CRT `rand()` (docs/RNG.md): MSVC LCG `state*214013+2531011`,
# output `(state >> 16) & 0x7FFF`, seeded from `time(nullptr)` at init
# (`0x44A491`).  Goldens are the documented `srand(1)` sequence.
add_executable(gm_rng_tests test_rng.cpp)
target_link_libraries(gm_rng_tests PRIVATE gm_engine)
add_test(NAME rng COMMAND gm_rng_tests)
gm_label(rng quick)

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
set(GM_CHECK_DEPENDS gm_tests gm_sou2_tests gm_sou1_tests gm_sof_tests gm_hud_tests gm_sbv_video_tests gm_sbtvm_tests gm_vm_tests gm_fuzz_tests gm_save_tests gm_doors_tests gunmetal)
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

# SECTOR_STUFF reader (docs/RECORDS.md §2, docs/WORLD_FIELDS.md): typed
# 256 x 0x58 slots instead of skip(0x5800).  Synthetic bounds/truncation
# always; with the install, 36/36 levels without type 8 and saved0/saved1
# block 8 field-for-field against gm/sbt.py SectorSlot.
add_executable(gm_sector_stuff_tests test_sector_stuff.cpp)
target_link_libraries(gm_sector_stuff_tests PRIVATE gm_engine)
add_test(NAME sector_stuff COMMAND gm_sector_stuff_tests)
gm_label(sector_stuff quick)
