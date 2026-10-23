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

# M5.2.5: music banks + overlay text kept as VM state (synthetic byte code).
add_executable(gm_vm_music_tests test_vm_music.cpp)
target_link_libraries(gm_vm_music_tests PRIVATE gm_engine)
# M7.3: engine 3D audio maths (distance, gain, pan, channel gains) against the
# vectors from tools/re/audio_3d.py.
add_executable(gm_engine_3d_tests test_engine_3d.cpp)
target_link_libraries(gm_engine_3d_tests PRIVATE gm_engine)
add_test(NAME engine_3d COMMAND gm_engine_3d_tests)
gm_label(engine_3d quick)

add_test(NAME vm_music COMMAND gm_vm_music_tests)
gm_label(vm_music quick)

# M9.1: the mission counter ds:0x80f9cc (NEXT_LEVEL / NEXT_LEVEL_NUMBER).
add_executable(gm_vm_campaign_tests test_vm_campaign.cpp)
target_link_libraries(gm_vm_campaign_tests PRIVATE gm_engine)
add_test(NAME vm_campaign COMMAND gm_vm_campaign_tests)
gm_label(vm_campaign quick)

# M6.5.2: the exact 0x453b50 intake against the numbers of docs/DAMAGE.md
# (45/12/3 hits, spill, flags 1/2/4, blast falloff).
add_executable(gm_damage_model_tests test_damage_model.cpp)
target_link_libraries(gm_damage_model_tests PRIVATE gm_engine)
add_test(NAME damage_model COMMAND gm_damage_model_tests)
gm_label(damage_model quick)

# M8.2/M9.5: full-screen requests recorded from the script (synthetic).
add_executable(gm_vm_screens_tests test_vm_screens.cpp)
target_link_libraries(gm_vm_screens_tests PRIVATE gm_engine)
add_test(NAME vm_screens COMMAND gm_vm_screens_tests)
gm_label(vm_screens quick)

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

# M12.5: a wrong --game must name the directory and the hint, not ".sbw not
# found".  Needs no installation.
add_test(NAME cli_no_game COMMAND gunmetal -g /nope --level level0 --info)
set_tests_properties(cli_no_game PROPERTIES PASS_REGULAR_EXPRESSION
    "game: directory not found: /nope")
gm_label(cli_no_game quick)

# M12.8/DoD: a missing resource is an `[error]` with rc=2, never a silent 0.
# `PASS_REGULAR_EXPRESSION` makes CTest accept the non-zero exit code.
add_test(NAME cli_missing_dump COMMAND gunmetal --dump /nope/x.sbt)
set_tests_properties(cli_missing_dump PROPERTIES PASS_REGULAR_EXPRESSION
    "no such file: /nope/x.sbt")
gm_label(cli_missing_dump quick)

if(EXISTS "/mnt/d/games/Gunmetal 3dfx/level0.sbt" AND GM_PYTHON3)
    # A wrong level name names the missing world file and says what else is
    # absent instead of failing on the first `.sbw` lookup alone.
    add_test(NAME cli_missing_level COMMAND gunmetal -l nosuchlevel --info)
    set_tests_properties(cli_missing_level PROPERTIES PASS_REGULAR_EXPRESSION
        "wrong level name")
    gm_label(cli_missing_level quick)
    # M11.1: text -> bytecode -> bytes on every shipped block.
    add_test(NAME sbtasm_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtasm.py
                     --roundtrip)
    set_tests_properties(sbtasm_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "15061/15061 byte-identical")
    gm_label(sbtasm_roundtrip quick)
    # M11.5: read -> write -> identical, and a block replacement that shifts
    # the sub-script offsets.
    add_test(NAME sbtwrite_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtwrite.py
                     --roundtrip)
    set_tests_properties(sbtwrite_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "identical: 36/36")
    gm_label(sbtwrite_roundtrip quick)
    add_test(NAME sbtwrite_replace
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtwrite.py
                     --verify-replace)
    set_tests_properties(sbtwrite_replace PROPERTIES PASS_REGULAR_EXPRESSION
        "others identical: yes, block==asm: yes")
    gm_label(sbtwrite_replace quick)
    # M11.7: the whole modder chain ends in the engine: disasm -> edit -> asm ->
    # replace in the container -> `gunmetal -l level0 --info` must load it.
    foreach(modlevel "level0;0x6f92" "dm1;0x666e")
        list(GET modlevel 0 mod_name)
        list(GET modlevel 1 mod_block)
        add_test(NAME mod_regress_${mod_name}
                 COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/mod_regress.py
                         --level ${mod_name} --block ${mod_block}
                         --work ${CMAKE_BINARY_DIR}/mod-regress-${mod_name})
        set_tests_properties(mod_regress_${mod_name} PROPERTIES
            PASS_REGULAR_EXPRESSION "result: ok")
        gm_label(mod_regress_${mod_name} quick)
    endforeach()
    # M11.6: every shipped BIGFILE pair repacked byte for byte.
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bigpack-tmp")
    add_test(NAME bigpack_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/bigpack.py
                     --roundtrip --tmp-dir ${CMAKE_BINARY_DIR}/bigpack-tmp)
    set_tests_properties(bigpack_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "all identical=True")
    gm_label(bigpack_roundtrip quick)
    # M11.3: every scene record survives read -> write -> compare.
    add_test(NAME sceneedit_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sceneedit.py
                     --roundtrip)
    set_tests_properties(sceneedit_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "identical \\(read -> write -> compare\\): 36/36")
    gm_label(sceneedit_roundtrip quick)
    # M11.2: every world survives read -> write -> compare, and the field
    # ranges of the shipped corpus are inside the documented bounds.
    add_test(NAME sbwedit_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwedit.py
                     --roundtrip)
    set_tests_properties(sbwedit_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "round-trip: 36/36 identical")
    gm_label(sbwedit_roundtrip quick)
    add_test(NAME sbwedit_audit
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwedit.py
                     --audit)
    set_tests_properties(sbwedit_audit PROPERTIES PASS_REGULAR_EXPRESSION
        "result: OK")
    gm_label(sbwedit_audit quick)
endif()

if(EXISTS "/mnt/d/games/Gunmetal 3dfx/sounds/SOU.IDX")
    # M7.6: the engine's own name lookup in the real binary.
    add_test(NAME cli_sample COMMAND gunmetal --sample alarmk)
    set_tests_properties(cli_sample PROPERTIES PASS_REGULAR_EXPRESSION
        "archive=sounds name=ALARMK offset=24369 size=13619")
    gm_label(cli_sample quick)
    add_test(NAME cli_sample_miss COMMAND gunmetal --sample nosuchname)
    set_tests_properties(cli_sample_miss PROPERTIES PASS_REGULAR_EXPRESSION
        "not found \\(the original plays silence\\)")
    gm_label(cli_sample_miss quick)
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

    # LIGHT chassis (TICK.sob) on the level0 spawn alcove floor, camera behind
    # it.  The shell's interior floor is z = 16 (the spawn at z = 48 floats 32
    # units above it); the boom is shortened so it stays inside the room.
    add_test(NAME cli_third_person COMMAND gunmetal --level level0 --third-person
        --third-pos 24,-208,16 --third-yaw 90 --third-distance 60
        --third-height 30 --fov 60 --render
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

# The `.sbw` object transform codec (docs/WORLD_FIELDS.md §3): 12 f64 =
# origin + 3x3 basis, `world = origin + basis * local`.  Every shipped object
# (36 levels, 28 192 objects, 338 304 header words) must decode with a zero low
# dword and an identity basis, and a synthetic rotated/scaled basis must match
# the Python `gm/sbw.py::Object.world_vertices` values to 1e-12.
add_executable(gm_sbw_transform_tests test_sbw_transform.cpp)
target_link_libraries(gm_sbw_transform_tests PRIVATE gm_engine)
add_test(NAME sbw_transform COMMAND gm_sbw_transform_tests)
gm_label(sbw_transform quick)

add_executable(gm_collide_tests test_collide.cpp)
target_link_libraries(gm_collide_tests PRIVATE gm_engine)
# Ray/sphere casts over .sbw triangles (docs/COLLIDE.md): cube, floor and
# two standalone triangles for exact t/point/normal, misses, parallel rays,
# origins inside, radius/edge/vertex contacts, degenerate polygons, NaN/inf,
# plus level0 floor/wall/AABB goldens from the Python oracle
# (tools/re/collide_goldens.py).
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
# KEYCFG/*.KEY are user data (the game rewrites them on remap), so the exact
# shipped values are pinned only while the files still match the 1.36
# manifest; a remapped install checks the parser invariants instead.
set(_keycfg_shipped TRUE)
foreach(_pair "PERFECT.key;3cbf40d9742e4589545074b8c7e121809e7dfd81e138bb55f59cf3fa56bf56f5"
              "SPLIT1.KEY;9f9798ec4ebb3c85b5cf0c8df26aea607b03c2cd7dc83d62a6dda7b9e97b058d"
              "SPLIT2.KEY;6025740645e5329b9fbebae6ce0f184e22b6e61fd92457edf0c652c7a4ee89c3")
    list(GET _pair 0 _name)
    list(GET _pair 1 _sha)
    if(EXISTS "/mnt/d/games/Gunmetal 3dfx/KEYCFG/${_name}")
        file(SHA256 "/mnt/d/games/Gunmetal 3dfx/KEYCFG/${_name}" _actual)
        if(NOT _actual STREQUAL _sha)
            set(_keycfg_shipped FALSE)
        endif()
    else()
        set(_keycfg_shipped FALSE)
    endif()
endforeach()
if(_keycfg_shipped)
    set_tests_properties(controls PROPERTIES ENVIRONMENT "GM_KEYCFG_SHIPPED=1")
endif()

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
# The type children of 0x46d000 (the opt-in `spawn_type_children`) are pinned
# by the synthetic `test_type_children` and by level11 (37 children).
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

# Thing movement automata (docs/THING_FIELDS.md §4a, docs/AI_CPP.md §7):
# synthetic per-mode steps against a floor/ceiling/wall world (spin, avoid 2/3/4,
# turret 5, ram 13, seek 14/15, patrol 16 fire band, projectile 6 gravity and
# bounce, floor stick, impulse bounce, thruster, negatives, determinism) plus a
# 91-tick smoke run over every spawned level0/dm1 instance.
add_executable(gm_thing_motion_tests test_thing_motion.cpp)
target_link_libraries(gm_thing_motion_tests PRIVATE gm_engine)
add_test(NAME thing_motion COMMAND gm_thing_motion_tests)
gm_label(thing_motion quick)

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

# M5.3.2: the instruction trace must be identical in C++ and Python.
# `tools/re/vm_trace_check.py` runs the dumper and gm/sbtvm_exec.py and diffs.
add_executable(gm_vm_trace_dump vm_trace_dump.cpp)
target_link_libraries(gm_vm_trace_dump PRIVATE gm_engine)

# Interface scaling (M4.3.3): aspect-preserving letterbox and the two
# filters, on synthetic pixels (no window, no SDL).
# Full-screen haze fade (M4.2.8): alpha truncation and the over blend, byte
# for byte with gm/render.py::fade_rgb.
add_executable(gm_postfx_tests test_postfx.cpp)
target_link_libraries(gm_postfx_tests PRIVATE gm_engine)
add_test(NAME postfx COMMAND gm_postfx_tests)
gm_label(postfx quick)

add_executable(gm_scale_tests test_scale.cpp)
target_link_libraries(gm_scale_tests PRIVATE gm_engine)
add_test(NAME scale COMMAND gm_scale_tests)
gm_label(scale quick)

# Effect animation frames (M4.2.6): 0x41de7d formula + .sbw effect table.
add_executable(gm_effect_anim_tests test_effect_anim.cpp)
target_link_libraries(gm_effect_anim_tests PRIVATE gm_engine)
add_test(NAME effect_anim COMMAND gm_effect_anim_tests)
gm_label(effect_anim quick)

# SOUND_SAMPLE_TABLE (record 12): 300 counts + 300 nine-byte names.
# M7.6: sample-name lookup - the table names must resolve to SOU.IDX entries,
# a miss is a quiet nullopt (the original plays silence).
add_executable(gm_sound_catalog_tests test_sound_catalog.cpp)
target_link_libraries(gm_sound_catalog_tests PRIVATE gm_engine)
add_test(NAME sound_catalog COMMAND gm_sound_catalog_tests)
gm_label(sound_catalog quick)

add_executable(gm_sample_table_tests test_sample_table.cpp)
target_link_libraries(gm_sample_table_tests PRIVATE gm_engine)
add_test(NAME sample_table COMMAND gm_sample_table_tests)
gm_label(sample_table quick)

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

# Event-driven blast damage (docs/PROJECTILE_CPP.md §7): THING_TYPES
# collide/expire event id -> EventBlastRegistry -> apply_blast_damage for all
# eight players and the things, through the runtime bridge.  Synthetic worlds
# plus the real level0.sbt collide/expire ids of ROCKET.
add_executable(gm_blast_damage_tests test_blast_damage.cpp)
target_link_libraries(gm_blast_damage_tests PRIVATE gm_engine)
add_test(NAME blast_damage COMMAND gm_blast_damage_tests)
gm_label(blast_damage quick)

# Aimed switches (kind 0x101) through CollisionWorld: GameRuntime::aim_switch
# is the 0x44fc30 ray half, docs/DOORS_CPP.md §2.3.
add_executable(gm_aim_switch_tests test_aim_switch.cpp)
target_link_libraries(gm_aim_switch_tests PRIVATE gm_engine)
add_test(NAME aim_switch COMMAND gm_aim_switch_tests)
gm_label(aim_switch quick)

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
    # M5.3.2: the C++ and Python instruction traces must be identical
    # (tools/re/vm_trace_check.py runs the dumper and gm/sbtvm_exec.py).
    add_test(NAME vm_trace
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/re/vm_trace_check.py
                     --level level0 --blocks 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
                     --binary $<TARGET_FILE:gm_vm_trace_dump>)
    gm_label(vm_trace quick)
    add_test(NAME vm_trace_dm1
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/re/vm_trace_check.py
                     --level dm1 --blocks 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19
                     --binary $<TARGET_FILE:gm_vm_trace_dump>)
    gm_label(vm_trace_dm1 quick)
    add_test(NAME parity_levels
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/parity_levels.py
                     --gunmetal $<TARGET_FILE:gunmetal>)
    gm_label(parity_levels full)
endif()
if(GM_PYTHON3)
    # M4.4.6: the noise/shift-tolerant frame metric validates itself on
    # synthetic images (no game files needed).
    add_test(NAME frame_metric
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/re/frame_metric.py
                     --self-test)
    gm_label(frame_metric quick)
    # Reference tools that need no game files: their self-tests run in the
    # quick suite so a broken model is caught before it reaches a report.
    add_test(NAME re_vm_exec
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/re/vm_exec.py
                     --selftest)
    gm_label(re_vm_exec quick)
    foreach(tool audio_3d damage export_parity gameover nested_things
                objectives pickup records sound_names ui_screens ui_sounds
                underwater)
        add_test(NAME re_${tool}
                 COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/re/${tool}.py
                         --self-test)
        gm_label(re_${tool} quick)
    endforeach()
    # M12.3: performance snapshot tool; the metric maths runs without a game.
    add_test(NAME perf_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/perf.py
                     --self-test)
    gm_label(perf_selftest quick)

    # M11.2: polygon fields of the world (.sbw) - tools/editor/sbwedit.py.
    add_test(NAME sbwedit_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwedit.py
                     --self-test)
    gm_label(sbwedit_selftest quick)

    # M11.3: scene records (everything but SCRIPT) - tools/editor/sceneedit.py.
    add_test(NAME sceneedit_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sceneedit.py
                     --self-test)
    gm_label(sceneedit_selftest quick)

    # M11.6: BIGFILE packing/replacement (tools/editor/bigpack.py).
    add_test(NAME bigpack_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/bigpack.py
                     --self-test)
    gm_label(bigpack_selftest quick)

    # M11.1/M11.5: the container writer (tools/editor/sbtwrite.py).
    add_test(NAME sbtwrite_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtwrite.py
                     --self-test)
    gm_label(sbtwrite_selftest quick)

    # M11.1: text <-> bytecode for the SBT scripts (tools/editor/sbtasm.py).
    add_test(NAME sbtasm_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtasm.py
                     --self-test)
    gm_label(sbtasm_selftest quick)
    # Independent re-derivation of the numbers in the merged RE docs
    # (docs/AUDIT_NUMBERS.md); no game files needed for the self-test.
    add_test(NAME doc_numbers
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/doc_numbers.py
                     --self-test)
    gm_label(doc_numbers quick)
endif()
if(GM_PYTHON3 AND EXISTS "/mnt/d/games/Gunmetal 3dfx")
    # M3.2.7: Python exports the expected parsed values for the shipped corpus
    # (tools/re/export_parity.py), then the C++ readers verify them with no
    # Python in the loop.  The fixture file is the contract, so a parser that
    # drifts on either side fails with a concrete key=value mismatch.
    add_executable(gm_parity_tests test_parity.cpp)
    target_link_libraries(gm_parity_tests PRIVATE gm_engine)
    add_test(NAME parity_fixtures
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/re/export_parity.py
                     --out ${CMAKE_BINARY_DIR}/parity/fixtures.txt)
    set_tests_properties(parity_fixtures PROPERTIES FIXTURES_SETUP parity)
    gm_label(parity_fixtures full)
    add_test(NAME parity
             COMMAND gm_parity_tests ${CMAKE_BINARY_DIR}/parity/fixtures.txt)
    set_tests_properties(parity PROPERTIES FIXTURES_REQUIRED parity)
    gm_label(parity full)

    # Tools that read the installation: same idea, one test each.
    foreach(tool campaign_flow dm_rules economy effects equipment
                hud_compose_ref hud_icons oracle_fit oracle_render screen_fx
                sector_stuff)
        add_test(NAME re_${tool}
                 COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/re/${tool}.py
                         --self-test)
        gm_label(re_${tool} quick)
    endforeach()
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

# Save block 8 as a typed, writable block (docs/RECORDS.md §2): saved0/saved1
# field-for-field against gm/sbt.py SectorSlot / tools/re/records.py, the
# 0x5800 payload re-encoded from the typed fields, one-field edits at exact
# byte offsets, an all-zero block and truncation negatives.
add_executable(gm_save_sector_stuff_tests test_save_sector_stuff.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/res/save.cpp)
target_link_libraries(gm_save_sector_stuff_tests PRIVATE gm_engine)
add_test(NAME save_sector_stuff COMMAND gm_save_sector_stuff_tests)
gm_label(save_sector_stuff quick)
