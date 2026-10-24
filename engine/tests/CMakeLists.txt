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

# M8: the headless frontend needs no game files (docs/FRONTEND.md).
# M8.1: the headless pager over text_layout pages (game only for the fonts).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/SML/help.sme")
    add_test(NAME cli_pager
             COMMAND gunmetal --pager help.sme)
    set_tests_properties(cli_pager PROPERTIES PASS_REGULAR_EXPRESSION
        "page 1/3 pages=3 lines=42")
    gm_label(cli_pager quick)
    add_test(NAME cli_pager_keys
             COMMAND gunmetal --pager credits.sml --pager-keys "next,next,prev")
    set_tests_properties(cli_pager_keys PROPERTIES PASS_REGULAR_EXPRESSION
        "final: page 2/5")
    gm_label(cli_pager_keys quick)
    add_test(NAME cli_pager_bad
             COMMAND gunmetal --pager credits.sml --page 99)
    set_tests_properties(cli_pager_bad PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] pager: no page 99")
    gm_label(cli_pager_bad quick)
    # M8.1 slice 3: `--png` renders the current page (after --page/--pager-keys)
    # into a 320x200 PNG (docs/PAGER_RENDER.md).
    add_test(NAME cli_pager_png
             COMMAND gunmetal --pager help.sme --page 2 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/pager-page2.png)
    set_tests_properties(cli_pager_png PROPERTIES PASS_REGULAR_EXPRESSION
        "render: page 2/3 320x200 lines=[0-9]+ glyphs=[0-9]+ missing=0 ink=[1-9][0-9]*")
    gm_label(cli_pager_png quick)
    add_test(NAME cli_pager_png_all
             COMMAND gunmetal --pager help.sme --all --png
                     ${CMAKE_CURRENT_BINARY_DIR}/pager-all.png)
    set_tests_properties(cli_pager_png_all PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] pager: --all prints every page")
    gm_label(cli_pager_png_all quick)
    # M8.1 slice 4: HELPSCRN behind the text and the fade level of 0x682f90.
    # Numbers are those of the module test (`pager_render`); the plain path
    # above is unchanged byte for byte.
    add_test(NAME cli_pager_png_background
             COMMAND gunmetal --pager help.sme --page 2 --background --png
                     ${CMAKE_CURRENT_BINARY_DIR}/pager-page2-bg.png)
    set_tests_properties(cli_pager_png_background PROPERTIES
        PASS_REGULAR_EXPRESSION
        "render: page 2/3 320x200 lines=15 glyphs=322 missing=0 ink=3905 background=64000")
    gm_label(cli_pager_png_background quick)
    add_test(NAME cli_pager_png_fade
             COMMAND gunmetal --pager help.sme --page 2 --background --fade 5
                     --png ${CMAKE_CURRENT_BINARY_DIR}/pager-page2-bg-fade.png)
    set_tests_properties(cli_pager_png_fade PROPERTIES PASS_REGULAR_EXPRESSION
        "render: page 2/3 320x200 .* ink=3905 background=64000 contrast=[0-9]+\\.\\.[0-9]+")
    gm_label(cli_pager_png_fade quick)
    # M8.5 slice 3: `--png-scale N` repeats each pixel as an N x N block
    # (render/upscale.h).  Mixed with the fade path: fade runs on the 320x200
    # frame first, the zoom is presentation only.
    add_test(NAME cli_pager_png_scale
             COMMAND gunmetal --pager help.sme --page 2 --background --fade 5
                     --png-scale 2
                     --png ${CMAKE_CURRENT_BINARY_DIR}/pager-page2-bg-fade-x2.png)
    set_tests_properties(cli_pager_png_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "scale: x2 640x400 \\(64000 -> 256000 pixels\\)")
    gm_label(cli_pager_png_scale quick)
    add_test(NAME cli_pager_png_scale_bad
             COMMAND gunmetal --pager help.sme --png-scale 9 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/pager-bad-scale.png)
    set_tests_properties(cli_pager_png_scale_bad PROPERTIES PASS_REGULAR_EXPRESSION
        "usage:")
    gm_label(cli_pager_png_scale_bad quick)
endif()

# M7.5 slice 3: `--playlist FILE` resolves a `.sog` against MUSIC/SOU.IDX
# (audio/playlist.*, docs/PLAYLIST.md).  Numbers are the module test's
# (`playlist`): Dm1B.sog has 8 entries, 7 found, 236876 bytes.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/MUSIC/SOU.IDX")
    add_test(NAME cli_playlist
             COMMAND gunmetal --playlist Dm1B.sog)
    set_tests_properties(cli_playlist PROPERTIES PASS_REGULAR_EXPRESSION
        "playlist: Dm1B.sog entries=8 found=7 missing=1 bytes=236876")
    gm_label(cli_playlist quick)
    add_test(NAME cli_playlist_missing
             COMMAND gunmetal --playlist Dm1B.sog)
    set_tests_properties(cli_playlist_missing PROPERTIES PASS_REGULAR_EXPRESSION
        "\\(missing\\)")
    gm_label(cli_playlist_missing quick)
endif()

# M8.4 slice 5: `--best-times` prints GUNMETAL.HI (ui/records.*,
# docs/HI_SCORES.md).  Numbers are the module test's (`records`): 9 rows,
# 7 distinct names, the minimum per name is 18 for TESTING RANGE.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/GUNMETAL.HI")
    add_test(NAME cli_best_times
             COMMAND gunmetal --best-times)
    set_tests_properties(cli_best_times PROPERTIES PASS_REGULAR_EXPRESSION
        "records: rows=9")
    gm_label(cli_best_times quick)
    add_test(NAME cli_best_times_best
             COMMAND gunmetal --best-times)
    set_tests_properties(cli_best_times_best PROPERTIES PASS_REGULAR_EXPRESSION
        "TESTING RANGE +18")
    gm_label(cli_best_times_best quick)
endif()

# M8.4 slice 4: `--killed NAME` prints the KILLED screen (ui/killed.*,
# docs/KILLED.md).  Numbers are the module test's (`killed`): .skb has 69
# accepted lines and 138 placeholder substitutions; seed 999 picks line 33.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/KILLED.skb")
    add_test(NAME cli_killed
             COMMAND gunmetal --killed PLAYER --killed-seed 999)
    set_tests_properties(cli_killed PROPERTIES PASS_REGULAR_EXPRESSION
        "killed: FRONTEND/KILLED.skb lines=69 subs=138 seed=999")
    gm_label(cli_killed quick)
    add_test(NAME cli_killed_all
             COMMAND gunmetal --killed PLAYER --killed-all)
    set_tests_properties(cli_killed_all PROPERTIES PASS_REGULAR_EXPRESSION
        "PLAYER MUTILATES PLAYER")
    gm_label(cli_killed_all quick)
endif()

# M4.4.5 slice 4: `--hud-png OUT` composes the cockpit frame
# (render/cockpit.h, docs/COCKPIT.md).  Numbers are the module test's
# (`cockpit`: elements/ink/panel for armour 10000, ammo 2000, kills 3).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_hud_png
             COMMAND gunmetal --hud-png ${CMAKE_CURRENT_BINARY_DIR}/hud.png
                     --hud-weapon 0 --hud-ammo 2000 --hud-kills 3)
    set_tests_properties(cli_hud_png PROPERTIES PASS_REGULAR_EXPRESSION
        "hud: 320x200 elements=[0-9]+ missing=0 ink=765 panel=21196")
    gm_label(cli_hud_png quick)
    add_test(NAME cli_hud_png_scale
             COMMAND gunmetal --hud-png ${CMAKE_CURRENT_BINARY_DIR}/hud-x2.png
                     --hud-weapon 0 --png-scale 2)
    set_tests_properties(cli_hud_png_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "scale: x2 640x400 \\(64000 -> 256000 pixels\\)")
    gm_label(cli_hud_png_scale quick)
    add_test(NAME cli_hud_png_fade
             COMMAND gunmetal --hud-png ${CMAKE_CURRENT_BINARY_DIR}/hud-fade.png
                     --hud-weapon 0 --fade 5)
    set_tests_properties(cli_hud_png_fade PROPERTIES PASS_REGULAR_EXPRESSION
        "fade: level 5 \\(max 11\\)")
    gm_label(cli_hud_png_fade quick)
endif()

# M8.3 slice 2: `--briefing LEVEL` reads SML/brief/<level>.sme
# (ui/briefing.*, docs/BRIEFING.md).  Numbers are those of the module test
# (`briefing`): level1 = 2 pages / 20 lines / 910 glyphs.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/SML/brief/level1.sme")
    add_test(NAME cli_briefing
             COMMAND gunmetal --briefing level1)
    set_tests_properties(cli_briefing PROPERTIES PASS_REGULAR_EXPRESSION
        "briefing: level1 .* pages=2 lines=20 glyphs=910")
    gm_label(cli_briefing quick)
    add_test(NAME cli_briefing_text
             COMMAND gunmetal --briefing level1 --briefing-text)
    set_tests_properties(cli_briefing_text PROPERTIES PASS_REGULAR_EXPRESSION
        "Morning, Soldier. Lieutenant Mills here, Dispatch.")
    gm_label(cli_briefing_text quick)
    add_test(NAME cli_briefing_missing
             COMMAND gunmetal --briefing nosuchlevel)
    set_tests_properties(cli_briefing_missing PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] briefing:")
    gm_label(cli_briefing_missing quick)
    # M8.3 slice 4: `--briefing LEVEL --png OUT [--page N]` renders one page
    # through the pager (ui/briefing_render.*, docs/BRIEFING_RENDER.md).
    # Numbers are the module test's: level1 page 1 = 15 lines / 719 glyphs /
    # ink 7214.
    add_test(NAME cli_briefing_png
             COMMAND gunmetal --briefing level1 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/briefing-level1.png)
    set_tests_properties(cli_briefing_png PROPERTIES PASS_REGULAR_EXPRESSION
        "render: page 1/2 320x200 lines=15 glyphs=719 missing=0 ink=7214")
    gm_label(cli_briefing_png quick)
    add_test(NAME cli_briefing_png_page2
             COMMAND gunmetal --briefing level1 --page 2 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/briefing-level1-p2.png)
    set_tests_properties(cli_briefing_png_page2 PROPERTIES PASS_REGULAR_EXPRESSION
        "render: page 2/2 320x200")
    gm_label(cli_briefing_png_page2 quick)
    add_test(NAME cli_briefing_png_bad_page
             COMMAND gunmetal --briefing level1 --page 99 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/briefing-bad.png)
    set_tests_properties(cli_briefing_png_bad_page PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] briefing:")
    gm_label(cli_briefing_png_bad_page quick)
    add_test(NAME cli_briefing_png_scale
             COMMAND gunmetal --briefing level1 --png-scale 2 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/briefing-level1-x2.png)
    set_tests_properties(cli_briefing_png_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "scale: x2 640x400")
    gm_label(cli_briefing_png_scale quick)
endif()

# M12.x: `--help` must list every parser option family (docs/CLI.md).  The
# assertion is a regex on the whole help screen, so it fails as soon as a
# documented flag disappears from `usage()`.
add_test(NAME cli_help COMMAND gunmetal --help)
set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION
    "--hud-opacity")
gm_label(cli_help quick)

# M8.5 slice 5: exact fade tables through the CLI.  Level 11 must reproduce
# the unfaded frame byte for byte, level 13 must differ (the model clamps
# 12..15 to 11).  The dump is an external artefact of this machine.
if(EXISTS "$ENV{HOME}/.cache/gunmetal-re/hud_lut.bin")
    add_test(NAME cli_fade_lut
             COMMAND ${CMAKE_COMMAND}
                     -DGUNMETAL=$<TARGET_FILE:gunmetal>
                     -DGAME=/mnt/d/games/Gunmetal\ 3dfx
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/fade-lut
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/fade_lut_check.cmake)
    gm_label(cli_fade_lut quick)
    # Same two facts with **no dump**: the tables are built from GAMEPAL
    # (`build_hud_lut`, docs/HUD_LUT.md §2b), so the exact fade no longer needs
    # an external file.  `GM_HUD_LUT` points at a missing path on purpose.
    add_test(NAME cli_fade_lut_built
             COMMAND ${CMAKE_COMMAND}
                     -DGUNMETAL=$<TARGET_FILE:gunmetal>
                     -DGAME=/mnt/d/games/Gunmetal\ 3dfx
                     -DLUT_ENV=${CMAKE_CURRENT_BINARY_DIR}/absent-hud-lut.bin
                     -DOUT=${CMAKE_CURRENT_BINARY_DIR}/fade-lut-built
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/fade_lut_check.cmake)
    gm_label(cli_fade_lut_built quick)
endif()

# M8.8 slice 1-2: screens 19 and 23 as PNG frames
# (ui/game_type_render.*, ui/difficulty_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_game_type
             COMMAND gunmetal --game-type)
    set_tests_properties(cli_game_type PROPERTIES PASS_REGULAR_EXPRESSION
        "game_type: mask=0xff rows=12 modes=11 cancel_row=106 highlight=0 arrows=0 ink=2478")
    gm_label(cli_game_type quick)
    add_test(NAME cli_game_type_mask
             COMMAND gunmetal --game-type --game-type-mask 0x01)
    set_tests_properties(cli_game_type_mask PROPERTIES PASS_REGULAR_EXPRESSION
        "mask=0x01 rows=4 modes=3 cancel_row=42")
    gm_label(cli_game_type_mask quick)
    add_test(NAME cli_game_type_png
             COMMAND gunmetal --game-type --png
                     ${CMAKE_CURRENT_BINARY_DIR}/game-type.png)
    set_tests_properties(cli_game_type_png PROPERTIES PASS_REGULAR_EXPRESSION
        "png:")
    gm_label(cli_game_type_png quick)
    add_test(NAME cli_difficulty
             COMMAND gunmetal --difficulty)
    set_tests_properties(cli_difficulty PROPERTIES PASS_REGULAR_EXPRESSION
        "difficulty: rows=6 cursor=2 ink=768 missing=0 arrows=0")
    gm_label(cli_difficulty quick)
    add_test(NAME cli_difficulty_cursor
             COMMAND gunmetal --difficulty --difficulty-cursor 5)
    set_tests_properties(cli_difficulty_cursor PROPERTIES PASS_REGULAR_EXPRESSION
        "difficulty: rows=6 cursor=5")
    gm_label(cli_difficulty_cursor quick)
endif()

# M8.8 slice 3: screen 21 as a PNG frame (ui/local_players_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_local_players
             COMMAND gunmetal --local-players)
    set_tests_properties(cli_local_players PROPERTIES PASS_REGULAR_EXPRESSION
        "local_players: rows=3 cursor=0 ink=936 missing=0 arrows=0")
    gm_label(cli_local_players quick)
    add_test(NAME cli_local_players_png
             COMMAND gunmetal --local-players --local-players-cursor 1 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/local-players.png)
    set_tests_properties(cli_local_players_png PROPERTIES PASS_REGULAR_EXPRESSION
        "local_players: rows=3 cursor=1")
    gm_label(cli_local_players_png quick)
endif()

# M8.8 slice 4: screen 33 as a PNG frame (ui/quit_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_quit
             COMMAND gunmetal --quit)
    set_tests_properties(cli_quit PROPERTIES PASS_REGULAR_EXPRESSION
        "quit: in_game=0 rows=2 cursor=0 ink=228 missing=0 arrows=0")
    gm_label(cli_quit quick)
    add_test(NAME cli_quit_in_game
             COMMAND gunmetal --quit --quit-in-game --quit-cursor 2)
    set_tests_properties(cli_quit_in_game PROPERTIES PASS_REGULAR_EXPRESSION
        "quit: in_game=1 rows=3 cursor=2 ink=576")
    gm_label(cli_quit_in_game quick)
    add_test(NAME cli_quit_png
             COMMAND gunmetal --quit --png
                     ${CMAKE_CURRENT_BINARY_DIR}/quit.png)
    set_tests_properties(cli_quit_png PROPERTIES PASS_REGULAR_EXPRESSION
        "png:")
    gm_label(cli_quit_png quick)
endif()

# M8.8 slice 5: screen 37 as a PNG frame (ui/video_mode_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_video_mode
             COMMAND gunmetal --video-mode)
    set_tests_properties(cli_video_mode PROPERTIES PASS_REGULAR_EXPRESSION
        "video_mode: rows=2 cursor=0 ink=356 missing=0 arrows=0")
    gm_label(cli_video_mode quick)
    add_test(NAME cli_video_mode_png
             COMMAND gunmetal --video-mode --video-mode-cursor 1 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/video-mode.png)
    set_tests_properties(cli_video_mode_png PROPERTIES PASS_REGULAR_EXPRESSION
        "video_mode: rows=2 cursor=1")
    gm_label(cli_video_mode_png quick)
endif()

# M8.8 slice 6: screen 6 as a PNG frame (ui/restart_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_restart
             COMMAND gunmetal --restart)
    set_tests_properties(cli_restart PROPERTIES PASS_REGULAR_EXPRESSION
        "restart: rows=2 cursor=0 ink=585 missing=0 arrows=0")
    gm_label(cli_restart quick)
    add_test(NAME cli_restart_four
             COMMAND gunmetal --restart --restart-items 4 --restart-cursor 3)
    set_tests_properties(cli_restart_four PROPERTIES PASS_REGULAR_EXPRESSION
        "restart: rows=4 cursor=3 ink=1132")
    gm_label(cli_restart_four quick)
    add_test(NAME cli_restart_png
             COMMAND gunmetal --restart --restart-items 3 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/restart.png)
    set_tests_properties(cli_restart_png PROPERTIES PASS_REGULAR_EXPRESSION
        "restart: rows=3 cursor=0 ink=898")
    gm_label(cli_restart_png quick)
endif()

# M6.3.1: `--things-anim` binds the live runtime to clips/frames
# (world/thing_anim.*, docs/THING_ANIM_BIND.md).  The default context set is
# wider than the module test's `contexts = {{0,0}}`, hence 268 things here vs
# the test's 267; the binding counters are the same shape.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/level13.sbt")
    add_test(NAME cli_things_anim
             COMMAND gunmetal --things-anim --level level13
                     --things-anim-tick 210 --things-anim-limit 1)
    set_tests_properties(cli_things_anim PROPERTIES PASS_REGULAR_EXPRESSION
        "things_anim: level=level13 things=268 agents=140 bound=10 unbound=258 animated=2 still=8 tick=210")
    gm_label(cli_things_anim quick)
    add_test(NAME cli_things_anim_clip
             COMMAND gunmetal --things-anim --level level13
                     --things-anim-tick 210 --things-anim-limit 1)
    set_tests_properties(cli_things_anim_clip PROPERTIES PASS_REGULAR_EXPRESSION
        "clip=slump frame=8 tick=210 looping=1")
    gm_label(cli_things_anim_clip quick)
endif()

# M8.8 slices 7-8: screens 18 and 22 as PNG frames
# (ui/video_mode_list_render.*, ui/mouse_setup_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_video_mode_list
             COMMAND gunmetal --video-mode-list)
    set_tests_properties(cli_video_mode_list PROPERTIES PASS_REGULAR_EXPRESSION
        "video_mode_list: rows=15 first_row=0 cursor=0 arrows=1 ink=2838 missing=0")
    gm_label(cli_video_mode_list quick)
    add_test(NAME cli_video_mode_list_end
             COMMAND gunmetal --video-mode-list --video-mode-list-cursor 14)
    set_tests_properties(cli_video_mode_list_end PROPERTIES PASS_REGULAR_EXPRESSION
        "rows=15 first_row=5 cursor=14 arrows=1 ink=2816")
    gm_label(cli_video_mode_list_end quick)
    add_test(NAME cli_mouse_setup
             COMMAND gunmetal --mouse-setup)
    set_tests_properties(cli_mouse_setup PROPERTIES PASS_REGULAR_EXPRESSION
        "mouse_setup: rows=12 sliders=3 cursor=0 ink=3986 missing=0 arrows=0")
    gm_label(cli_mouse_setup quick)
    add_test(NAME cli_mouse_setup_png
             COMMAND gunmetal --mouse-setup --mouse-setup-cursor 11 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/mouse-setup.png)
    set_tests_properties(cli_mouse_setup_png PROPERTIES PASS_REGULAR_EXPRESSION
        "mouse_setup: rows=12 sliders=3 cursor=11")
    gm_label(cli_mouse_setup_png quick)
endif()

# M8.8 slice 10: screen 26 as a PNG frame (ui/joystick_centering_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_joystick_centering
             COMMAND gunmetal --joystick-centering)
    set_tests_properties(cli_joystick_centering PROPERTIES PASS_REGULAR_EXPRESSION
        "joystick_centering: rows=1 cursor=-1 ink=1141 missing=0 arrows=0")
    gm_label(cli_joystick_centering quick)
    add_test(NAME cli_joystick_centering_png
             COMMAND gunmetal --joystick-centering --joystick-centering-cursor 0
                     --png ${CMAKE_CURRENT_BINARY_DIR}/joystick-centering.png)
    set_tests_properties(cli_joystick_centering_png PROPERTIES PASS_REGULAR_EXPRESSION
        "joystick_centering: rows=1 cursor=0 ink=1141")
    gm_label(cli_joystick_centering_png quick)
endif()

# M8.8 slice 9: screen 27 as a PNG frame (ui/match_setup_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_match_setup
             COMMAND gunmetal --match-setup)
    set_tests_properties(cli_match_setup PROPERTIES PASS_REGULAR_EXPRESSION
        "match_setup: rows=8 sliders=0 checks=5 flags=0x00 cursor=7 ink=2239 missing=0 arrows=0")
    gm_label(cli_match_setup quick)
    add_test(NAME cli_match_setup_checks
             COMMAND gunmetal --match-setup --match-setup-flags 0x75
                     --match-setup-cursor 0)
    set_tests_properties(cli_match_setup_checks PROPERTIES PASS_REGULAR_EXPRESSION
        "match_setup: rows=8 sliders=0 checks=5 flags=0x75 cursor=0 ink=2362")
    gm_label(cli_match_setup_checks quick)
    add_test(NAME cli_match_setup_png
             COMMAND gunmetal --match-setup --match-setup-type 2
                     --match-setup-cash 11 --match-setup-flags 0x75 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/match-setup.png)
    set_tests_properties(cli_match_setup_png PROPERTIES PASS_REGULAR_EXPRESSION
        "png:")
    gm_label(cli_match_setup_png quick)
endif()

# M8.8 slice 11: screen 16 as a PNG frame (ui/key_jamming_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_key_jamming
             COMMAND gunmetal --key-jamming)
    set_tests_properties(cli_key_jamming PROPERTIES PASS_REGULAR_EXPRESSION
        "key_jamming: rows=1 keys=0 cursor=4 ink=752 missing=0 arrows=0")
    gm_label(cli_key_jamming quick)
    add_test(NAME cli_key_jamming_keys
             COMMAND gunmetal --key-jamming --key-jamming-keys 17,30,31,57,109
                     --png ${CMAKE_CURRENT_BINARY_DIR}/key-jamming.png)
    set_tests_properties(cli_key_jamming_keys PROPERTIES PASS_REGULAR_EXPRESSION
        "key_jamming: rows=1 keys=5 cursor=4 ink=939")
    gm_label(cli_key_jamming_keys quick)
endif()

# M8.8 slice 12: screen 9 as a PNG frame (ui/ingame_menu_render.*).  The screen
# has no menu: it is one full-screen DEMOTION/DISBAND entry, so rows and ink are
# zero by proof, and the two backgrounds differ in 49323 of 64000 pixels.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_ingame_menu
             COMMAND gunmetal --ingame-menu)
    set_tests_properties(cli_ingame_menu PROPERTIES PASS_REGULAR_EXPRESSION
        "ingame_menu: bg=DEMOTION rows=0 ink=0 missing=0 arrows=0")
    gm_label(cli_ingame_menu quick)
    add_test(NAME cli_ingame_menu_disband
             COMMAND gunmetal --ingame-menu --ingame-menu-bg DISBAND --png
                     ${CMAKE_CURRENT_BINARY_DIR}/ingame-menu.png)
    set_tests_properties(cli_ingame_menu_disband PROPERTIES PASS_REGULAR_EXPRESSION
        "ingame_menu: bg=DISBAND rows=0 ink=0")
    gm_label(cli_ingame_menu_disband quick)
endif()

# LUT builder: `--hud-lut-dump` writes exactly the 4096 bytes the dump has, and
# refuses to write inside the installation (docs/LUT_BUILDER.md).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_hud_lut_dump
             COMMAND gunmetal --hud-lut-dump
                     ${CMAKE_CURRENT_BINARY_DIR}/hud_lut.bin)
    set_tests_properties(cli_hud_lut_dump PROPERTIES PASS_REGULAR_EXPRESSION
        "hud_lut: 4096 bytes, fnv1a64=0x6330406254771f3d")
    gm_label(cli_hud_lut_dump quick)
    add_test(NAME cli_hud_lut_dump_refuses_game
             COMMAND gunmetal --hud-lut-dump
                     "/mnt/d/games/Gunmetal 3dfx/gm-hud-lut-must-not-appear.bin")
    set_tests_properties(cli_hud_lut_dump_refuses_game PROPERTIES
        PASS_REGULAR_EXPRESSION "refusing to write inside the installation")
    gm_label(cli_hud_lut_dump_refuses_game quick)
endif()

# M8.8 slices 13-14: screens 15 and 20 as PNG frames
# (ui/save_name_render.*, ui/lay_bucks_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_lay_bucks
             COMMAND gunmetal --lay-bucks)
    set_tests_properties(cli_lay_bucks PROPERTIES PASS_REGULAR_EXPRESSION
        "lay_bucks: rows=1 ink=1036 missing=0 arrows=0")
    gm_label(cli_lay_bucks quick)
    add_test(NAME cli_lay_bucks_png
             COMMAND gunmetal --lay-bucks --png
                     ${CMAKE_CURRENT_BINARY_DIR}/lay-bucks.png)
    set_tests_properties(cli_lay_bucks_png PROPERTIES PASS_REGULAR_EXPRESSION
        "png:")
    gm_label(cli_lay_bucks_png quick)
    add_test(NAME cli_save_name
             COMMAND gunmetal --save-name)
    set_tests_properties(cli_save_name PROPERTIES PASS_REGULAR_EXPRESSION
        "save_name: rows=1 chars=13 truncated=0 row=0 ink=544 missing=0 arrows=0 bars=1")
    gm_label(cli_save_name quick)
    add_test(NAME cli_save_name_long
             COMMAND gunmetal --save-name --save-name-row 3 --save-name-text
                     "HELLO WORLD THIS IS LONG")
    set_tests_properties(cli_save_name_long PROPERTIES PASS_REGULAR_EXPRESSION
        "save_name: rows=1 chars=18 truncated=7 row=3 ink=570")
    gm_label(cli_save_name_long quick)
    add_test(NAME cli_save_name_no_caret
             COMMAND gunmetal --save-name --save-name-no-caret --no-fileslot
                     --png ${CMAKE_CURRENT_BINARY_DIR}/save-name.png)
    set_tests_properties(cli_save_name_no_caret PROPERTIES PASS_REGULAR_EXPRESSION
        "save_name: rows=1 chars=12 truncated=0 row=0 ink=539")
    gm_label(cli_save_name_no_caret quick)
endif()

# M8.8 slice 16: screen 5 as a PNG frame (ui/error_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_error_screen
             COMMAND gunmetal --error)
    set_tests_properties(cli_error_screen PROPERTIES PASS_REGULAR_EXPRESSION
        "error_screen: rows=0 lines=4 ink=489 missing=0 arrows=0")
    gm_label(cli_error_screen quick)
    add_test(NAME cli_error_screen_long
             COMMAND gunmetal --error --error-message
                     "Unable to load world file (savegame)." --png
                     ${CMAKE_CURRENT_BINARY_DIR}/error.png)
    set_tests_properties(cli_error_screen_long PROPERTIES PASS_REGULAR_EXPRESSION
        "error_screen: rows=0 lines=8 ink=803")
    gm_label(cli_error_screen_long quick)
endif()

# M8.8 slice 15: screen 36 as a PNG frame (ui/player_setup_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_player_setup
             COMMAND gunmetal --player-setup)
    set_tests_properties(cli_player_setup PROPERTIES PASS_REGULAR_EXPRESSION
        "player_setup: player=0 rows=1 sliders=0 checks=0 cursor=0 joystick=-1 arrows=0 ink=825 missing=0")
    gm_label(cli_player_setup quick)
    add_test(NAME cli_player_setup_device
             COMMAND gunmetal --player-setup --player-setup-joystick 0)
    set_tests_properties(cli_player_setup_device PROPERTIES PASS_REGULAR_EXPRESSION
        "player_setup: player=0 rows=43 sliders=0 checks=0 cursor=0 joystick=0 arrows=1 ink=2640")
    gm_label(cli_player_setup_device quick)
    add_test(NAME cli_player_setup_player2
             COMMAND gunmetal --player-setup --player-setup-joystick 2
                     --player-setup-player 1 --player-setup-cursor 10 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/player-setup.png)
    set_tests_properties(cli_player_setup_player2 PROPERTIES PASS_REGULAR_EXPRESSION
        "player_setup: player=1 rows=43 sliders=0 checks=0 cursor=10 joystick=2 arrows=2 ink=2404")
    gm_label(cli_player_setup_player2 quick)
endif()

# M8.8 slices 17-18: screens 10 and 11 as PNG frames
# (ui/control_options_render.*, ui/sound_options_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_control_options
             COMMAND gunmetal --control-options)
    set_tests_properties(cli_control_options PROPERTIES PASS_REGULAR_EXPRESSION
        "control_options: rows=10 sliders=0 checks=0 cursor=0 arrows=0 ink=1736 missing=0")
    gm_label(cli_control_options quick)
    add_test(NAME cli_control_options_values
             COMMAND gunmetal --control-options --control-options-keyset1 2
                     --control-options-joystick1 0 --control-options-mouse 2
                     --control-options-cursor 9 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/control-options.png)
    set_tests_properties(cli_control_options_values PROPERTIES PASS_REGULAR_EXPRESSION
        "control_options: rows=10 sliders=0 checks=0 cursor=9 arrows=0 ink=1472")
    gm_label(cli_control_options_values quick)
    add_test(NAME cli_sound_options
             COMMAND gunmetal --sound-options)
    set_tests_properties(cli_sound_options PROPERTIES PASS_REGULAR_EXPRESSION
        "sound_options: rows=7 sliders=2 checks=1 cursor=0 arrows=0 ink=1869 missing=0")
    gm_label(cli_sound_options quick)
    add_test(NAME cli_sound_options_off
             COMMAND gunmetal --sound-options --sound-options-off)
    set_tests_properties(cli_sound_options_off PROPERTIES PASS_REGULAR_EXPRESSION
        "sound_options: rows=1 sliders=0 checks=0 cursor=0 arrows=0 ink=1098")
    gm_label(cli_sound_options_off quick)
    add_test(NAME cli_sound_options_low_memory
             COMMAND gunmetal --sound-options --sound-options-low-memory --png
                     ${CMAKE_CURRENT_BINARY_DIR}/sound-options.png)
    set_tests_properties(cli_sound_options_low_memory PROPERTIES PASS_REGULAR_EXPRESSION
        "sound_options: rows=7 sliders=1 checks=1 cursor=0 arrows=0 ink=1524")
    gm_label(cli_sound_options_low_memory quick)
    add_test(NAME cli_sound_options_values
             COMMAND gunmetal --sound-options --sound-options-music 0
                     --sound-options-effects 0 --sound-options-swap
                     --sound-options-cursor 6)
    set_tests_properties(cli_sound_options_values PROPERTIES PASS_REGULAR_EXPRESSION
        "sound_options: rows=7 sliders=2 checks=1 cursor=6 arrows=0 ink=1966")
    gm_label(cli_sound_options_values quick)
endif()

# M8.8 slices 19-20: screens 12 and 32 as PNG frames
# (ui/graphics_options_render.*, ui/play_options_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_graphics_options
             COMMAND gunmetal --graphics-options)
    set_tests_properties(cli_graphics_options PROPERTIES PASS_REGULAR_EXPRESSION
        "graphics_options: rows=6 sliders=1 checks=1 cursor=0 arrows=0 ink=1866 missing=0")
    gm_label(cli_graphics_options quick)
    add_test(NAME cli_graphics_options_values
             COMMAND gunmetal --graphics-options --graphics-options-border 9
                     --graphics-options-gamma 3 --graphics-options-reflections 0
                     --graphics-options-light-maps 2 --graphics-options-cursor 5
                     --png ${CMAKE_CURRENT_BINARY_DIR}/graphics-options.png)
    set_tests_properties(cli_graphics_options_values PROPERTIES PASS_REGULAR_EXPRESSION
        "graphics_options: rows=6 sliders=1 checks=1 cursor=5 arrows=0 ink=1862")
    gm_label(cli_graphics_options_values quick)
    add_test(NAME cli_play_options
             COMMAND gunmetal --play-options)
    set_tests_properties(cli_play_options PROPERTIES PASS_REGULAR_EXPRESSION
        "play_options: rows=8 sliders=0 checks=6 cursor=0 arrows=0 ink=2765 missing=0")
    gm_label(cli_play_options quick)
    add_test(NAME cli_play_options_flags
             COMMAND gunmetal --play-options --play-options-flags 0xff
                     --play-options-camera 2 --play-options-cursor 7 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/play-options.png)
    set_tests_properties(cli_play_options_flags PROPERTIES PASS_REGULAR_EXPRESSION
        "play_options: rows=8 sliders=0 checks=6 cursor=7 arrows=0 ink=2737")
    gm_label(cli_play_options_flags quick)
    add_test(NAME cli_play_options_in_game
             COMMAND gunmetal --play-options --play-options-in-game)
    set_tests_properties(cli_play_options_in_game PROPERTIES PASS_REGULAR_EXPRESSION
        "play_options: rows=8 sliders=0 checks=6 cursor=0 arrows=0 ink=2854")
    gm_label(cli_play_options_in_game quick)
endif()

# M8.8 slices 22-23: screens 24/25 and 31 as PNG frames
# (ui/player_name_render.*, ui/new_keyset_render.*).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_player_name
             COMMAND gunmetal --player-name)
    set_tests_properties(cli_player_name PROPERTIES PASS_REGULAR_EXPRESSION
        "player_name: player=0 rows=1 chars=0 truncated=0 caret=1 caret_px=46 ink=1812 missing=0 arrows=0")
    gm_label(cli_player_name quick)
    add_test(NAME cli_player_name_two
             COMMAND gunmetal --player-name --player-name-player 1
                     --player-name-text "JIM BOB")
    set_tests_properties(cli_player_name_two PROPERTIES PASS_REGULAR_EXPRESSION
        "player_name: player=1 rows=1 chars=7 truncated=0 caret=1 caret_px=46 ink=2331")
    gm_label(cli_player_name_two quick)
    add_test(NAME cli_player_name_long
             COMMAND gunmetal --player-name --player-name-text
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234" --png
                     ${CMAKE_CURRENT_BINARY_DIR}/player-name.png)
    set_tests_properties(cli_player_name_long PROPERTIES PASS_REGULAR_EXPRESSION
        "player_name: player=0 rows=1 chars=10 truncated=20 caret=1 caret_px=46 ink=2617")
    gm_label(cli_player_name_long quick)
    add_test(NAME cli_new_keyset
             COMMAND gunmetal --new-keyset)
    set_tests_properties(cli_new_keyset PROPERTIES PASS_REGULAR_EXPRESSION
        "new_keyset: rows=1 chars=7 truncated=0 ink=1849 missing=0 arrows=0")
    gm_label(cli_new_keyset quick)
    add_test(NAME cli_new_keyset_long
             COMMAND gunmetal --new-keyset --new-keyset-text
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234" --png
                     ${CMAKE_CURRENT_BINARY_DIR}/new-keyset.png)
    set_tests_properties(cli_new_keyset_long PROPERTIES PASS_REGULAR_EXPRESSION
        "new_keyset: rows=1 chars=15 truncated=15 ink=2582")
    gm_label(cli_new_keyset_long quick)
    add_test(NAME cli_new_keyset_empty
             COMMAND gunmetal --new-keyset --new-keyset-text "")
    set_tests_properties(cli_new_keyset_empty PROPERTIES PASS_REGULAR_EXPRESSION
        "new_keyset: rows=1 chars=0 truncated=0 ink=1314")
    gm_label(cli_new_keyset_empty quick)
endif()

# M8.4 slice 4: `--level-select [--level-select-counter N] [--level-select-row N]
# [--no-plate] --png OUT` renders screen 17 (ui/level_select_render.*).
# Numbers are the module test's: counter 1 = 36 rows / 7 unlocked, ink 1305;
# counter 25 with row 19 is the window 14..23.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_level_select
             COMMAND gunmetal --level-select)
    set_tests_properties(cli_level_select PROPERTIES PASS_REGULAR_EXPRESSION
        "level_select: counter=1 rows=36 unlocked=7 selected=0 \\(level1\\)")
    gm_label(cli_level_select quick)
    add_test(NAME cli_level_select_png
             COMMAND gunmetal --level-select --png
                     ${CMAKE_CURRENT_BINARY_DIR}/level-select.png)
    set_tests_properties(cli_level_select_png PROPERTIES PASS_REGULAR_EXPRESSION
        "render: rows=10 selected=0 320x200 ink=1305 missing=0")
    gm_label(cli_level_select_png quick)
    add_test(NAME cli_level_select_scroll
             COMMAND gunmetal --level-select --level-select-counter 25
                     --level-select-row 19)
    set_tests_properties(cli_level_select_scroll PROPERTIES PASS_REGULAR_EXPRESSION
        "selected=19 \\(level20\\)")
    gm_label(cli_level_select_scroll quick)
    add_test(NAME cli_level_select_scale
             COMMAND gunmetal --level-select --png-scale 2 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/level-select-x2.png)
    set_tests_properties(cli_level_select_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "scale: x2 640x400")
    gm_label(cli_level_select_scale quick)
endif()

# M8.7 slice 4: `--save-menu [--save-menu-slot N] [--save-menu-mode load|save]
# [--no-plate] --png OUT` renders screen 14 (ui/save_menu_render.*).
# Numbers are the module test's: 20 slots, 2 occupied, 6 rows, ink 917;
# save mode slot 8 scrolls the window to 3..8.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_save_menu
             COMMAND gunmetal --save-menu)
    set_tests_properties(cli_save_menu PROPERTIES PASS_REGULAR_EXPRESSION
        "save_menu: mode=load slots=20 occupied=2 selected=0 first=0")
    gm_label(cli_save_menu quick)
    add_test(NAME cli_save_menu_png
             COMMAND gunmetal --save-menu --png
                     ${CMAKE_CURRENT_BINARY_DIR}/save-menu.png)
    set_tests_properties(cli_save_menu_png PROPERTIES PASS_REGULAR_EXPRESSION
        "render: rows=6 scroll=0 selected=0 slot_rows=2 320x200 ink=917")
    gm_label(cli_save_menu_png quick)
    add_test(NAME cli_save_menu_scroll
             COMMAND gunmetal --save-menu --save-menu-mode save
                     --save-menu-slot 8)
    set_tests_properties(cli_save_menu_scroll PROPERTIES PASS_REGULAR_EXPRESSION
        "selected=8 first=3")
    gm_label(cli_save_menu_scroll quick)
    add_test(NAME cli_save_menu_scale
             COMMAND gunmetal --save-menu --png-scale 2 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/save-menu-x2.png)
    set_tests_properties(cli_save_menu_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "scale: x2 640x400")
    gm_label(cli_save_menu_scale quick)
endif()

# M8.7 slice 2 (CLI): `--save-action load|save --save-menu-slot N` applies the
# menu action (game/save_action.*, docs/SAVE_ACTION.md).  Numbers are the
# module test's: slot 0 -> 15 blocks, sector_stuff 13, feet/sha of 243 368 B.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt")
    add_test(NAME cli_save_action_load
             COMMAND gunmetal --save-action load --save-menu-slot 0)
    set_tests_properties(cli_save_action_load PROPERTIES PASS_REGULAR_EXPRESSION
        "level=level0 title=\"A SAVED GAME\" feet=\\(1116.173752, 798.207621, 48.000000\\)")
    gm_label(cli_save_action_load quick)
    add_test(NAME cli_save_action_load_stats
             COMMAND gunmetal --save-action load --save-menu-slot 0)
    set_tests_properties(cli_save_action_load_stats PROPERTIES PASS_REGULAR_EXPRESSION
        "result=loaded saves=0 loads=1 refusals=0")
    gm_label(cli_save_action_load_stats quick)
    add_test(NAME cli_save_action_save
             COMMAND gunmetal --save-action save --save-menu-slot 0
                     --save-action-out ${CMAKE_CURRENT_BINARY_DIR}/save-action)
    set_tests_properties(cli_save_action_save PROPERTIES PASS_REGULAR_EXPRESSION
        "result=saved saves=1 loads=0 refusals=0")
    gm_label(cli_save_action_save quick)
    add_test(NAME cli_save_action_empty
             COMMAND gunmetal --save-action save --save-menu-slot 5
                     --save-action-out ${CMAKE_CURRENT_BINARY_DIR}/save-action)
    set_tests_properties(cli_save_action_empty PROPERTIES PASS_REGULAR_EXPRESSION
        "result=refused_empty")
    gm_label(cli_save_action_empty quick)
endif()

# M8.7 slice 5 (CLI): `--start-game [--save-menu-slot N] --png OUT` renders
# screen 13 (ui/start_game_render.*).  Numbers are the module test's:
# 8 rows, 6 slot rows, ink 1608, one arrow (the list can scroll down).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_start_game
             COMMAND gunmetal --start-game)
    set_tests_properties(cli_start_game PROPERTIES PASS_REGULAR_EXPRESSION
        "start_game: slots=20 occupied=2 selected=0 first=0")
    gm_label(cli_start_game quick)
    add_test(NAME cli_start_game_png
             COMMAND gunmetal --start-game --png
                     ${CMAKE_CURRENT_BINARY_DIR}/start-game.png)
    set_tests_properties(cli_start_game_png PROPERTIES PASS_REGULAR_EXPRESSION
        "render: rows=8 first_slot=0 slot_rows=6 320x200 ink=1608 arrows=1")
    gm_label(cli_start_game_png quick)
    add_test(NAME cli_start_game_scale
             COMMAND gunmetal --start-game --png-scale 2 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/start-game-x2.png)
    set_tests_properties(cli_start_game_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "scale: x2 640x400")
    gm_label(cli_start_game_scale quick)
endif()

# M8.7 slice 7 (CLI): `--keycfg NAME --png OUT` renders screen 30 from a real
# KEYCFG file (ui/keycfg_render.*).  Numbers are the module test's:
# PERFECT = 31 rows / 29 named / ink 2659; SPLIT1 = 19 named / ink 2508.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/KEYCFG/PERFECT.key")
    add_test(NAME cli_keycfg
             COMMAND gunmetal --keycfg PERFECT)
    set_tests_properties(cli_keycfg PROPERTIES PASS_REGULAR_EXPRESSION
        "keycfg: PERFECT.key rows=31 named=29 cursor=0 first_row=0 arrows=1 mode=view conflicts=0 ink=2659 missing=0")
    gm_label(cli_keycfg quick)
    add_test(NAME cli_keycfg_split1
             COMMAND gunmetal --keycfg SPLIT1)
    set_tests_properties(cli_keycfg_split1 PROPERTIES PASS_REGULAR_EXPRESSION
        "keycfg: SPLIT1.key rows=31 named=19 cursor=0 first_row=0 arrows=1 mode=view conflicts=0 ink=2508")
    gm_label(cli_keycfg_split1 quick)
    add_test(NAME cli_keycfg_png
             COMMAND gunmetal --keycfg PERFECT --png
                     ${CMAKE_CURRENT_BINARY_DIR}/keycfg.png)
    set_tests_properties(cli_keycfg_png PROPERTIES PASS_REGULAR_EXPRESSION
        "render: 320x200 ink=2659")
    gm_label(cli_keycfg_png quick)
    add_test(NAME cli_keycfg_cursor
             COMMAND gunmetal --keycfg PERFECT --keycfg-cursor 15)
    set_tests_properties(cli_keycfg_cursor PROPERTIES PASS_REGULAR_EXPRESSION
        "cursor=15 first_row=10 arrows=2 mode=view conflicts=0 ink=3181")
    gm_label(cli_keycfg_cursor quick)
    add_test(NAME cli_keycfg_edit
             COMMAND gunmetal --keycfg PERFECT --keycfg-mode edit --keycfg-cell b)
    set_tests_properties(cli_keycfg_edit PROPERTIES PASS_REGULAR_EXPRESSION
        "mode=edit conflicts=0 ink=2774")
    gm_label(cli_keycfg_edit quick)
    add_test(NAME cli_keycfg_conflict
             COMMAND gunmetal --keycfg PERFECT --keycfg-mode edit
                     --keycfg-conflict "MOVE FORWARD has been used twice")
    set_tests_properties(cli_keycfg_conflict PROPERTIES PASS_REGULAR_EXPRESSION
        "mode=edit conflicts=1 ink=3209")
    gm_label(cli_keycfg_conflict quick)
    add_test(NAME cli_keycfg_missing
             COMMAND gunmetal --keycfg no-such-profile)
    set_tests_properties(cli_keycfg_missing PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] keycfg: keycfg_render: cannot read")
    gm_label(cli_keycfg_missing quick)
endif()

# M8.2 slice 2: the current frontend screen as a PNG (panel from BMP.IDX).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/FRONTEND/BMP.IDX")
    add_test(NAME cli_menu_png
             COMMAND gunmetal --menu-screen 10 --menu-keys down,down
                     --png ${CMAKE_BINARY_DIR}/menu-control.png)
    set_tests_properties(cli_menu_png PROPERTIES PASS_REGULAR_EXPRESSION
        "render: CONTROL OPTIONS 320x200 items=10 drawn=10 ink=[1-9]")
    gm_label(cli_menu_png quick)
    # `--menu --png` without `--menu-keys` used to return before the picture;
    # the screen dump now writes it in both modes (bug found 2026-10-24).
    add_test(NAME cli_menu_png_plain
             COMMAND gunmetal --menu-screen 1 --png
                     ${CMAKE_CURRENT_BINARY_DIR}/menu-main.png)
    set_tests_properties(cli_menu_png_plain PROPERTIES PASS_REGULAR_EXPRESSION
        "render: MAIN MENU 320x200 items=7 drawn=7 ink=[1-9]")
    gm_label(cli_menu_png_plain quick)
    add_test(NAME cli_menu_png_fade
             COMMAND gunmetal --menu-screen 10 --fade 5
                     --png ${CMAKE_CURRENT_BINARY_DIR}/menu-control-fade.png)
    set_tests_properties(cli_menu_png_fade PROPERTIES PASS_REGULAR_EXPRESSION
        "fade: level 5 \\(max 11\\)")
    gm_label(cli_menu_png_fade quick)
    add_test(NAME cli_menu_png_fade_scale
             COMMAND gunmetal --menu-screen 10 --fade 5 --png-scale 2
                     --png ${CMAKE_CURRENT_BINARY_DIR}/menu-control-fade-x2.png)
    set_tests_properties(cli_menu_png_fade_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "fade: level 5 \\(max 11\\).*scale: x2 640x400")
    gm_label(cli_menu_png_fade_scale quick)
    add_test(NAME cli_menu_png_scale
             COMMAND gunmetal --menu-screen 10 --png-scale 3
                     --png ${CMAKE_CURRENT_BINARY_DIR}/menu-control-x3.png)
    set_tests_properties(cli_menu_png_scale PROPERTIES PASS_REGULAR_EXPRESSION
        "scale: x3 960x600 \\(64000 -> 576000 pixels\\)")
    gm_label(cli_menu_png_scale quick)
    add_test(NAME cli_menu_png_bad
             COMMAND gunmetal --game /nonexistent --menu --png
                     ${CMAKE_BINARY_DIR}/menu-bad.png)
    set_tests_properties(cli_menu_png_bad PROPERTIES PASS_REGULAR_EXPRESSION
        "menu|font|BMP")
    gm_label(cli_menu_png_bad quick)
endif()

# M8.7/M9.4: the strict save writer round-trips a shipped save byte for byte.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt")
    add_test(NAME cli_save_out
             COMMAND gunmetal --dump SAVEGAME/saved0.sbt
                     --save-out ${CMAKE_BINARY_DIR}/saved0-roundtrip.sbt)
    set_tests_properties(cli_save_out PROPERTIES PASS_REGULAR_EXPRESSION
        "blocks=15, byte-exact=yes")
    gm_label(cli_save_out quick)
endif()

# M8.7: the headless save-slot table (`docs/SAVE_SLOTS.md`).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/savegame/saved0.sbt")
    add_test(NAME cli_save_slots COMMAND gunmetal --save-slots)
    set_tests_properties(cli_save_slots PROPERTIES PASS_REGULAR_EXPRESSION
        "save-slots: present=2/20")
    gm_label(cli_save_slots quick)
endif()

# M7.5: the music envelope is applied to the decoded PCM (`docs/MUSIC.md`).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/MUSIC/Dm1A.sog")
    add_test(NAME cli_music_fade
             COMMAND gunmetal --music Dm1A --headless --fade-in 1.0 --fade-out 1.0)
    set_tests_properties(cli_music_fade PROPERTIES PASS_REGULAR_EXPRESSION
        "fade-in=1.000 fade-out=1.000 gain first=0.032000 mid=1.000000")
    gm_label(cli_music_fade quick)
    add_test(NAME cli_music_fade_bad COMMAND gunmetal --music Dm1A --fade-in -1)
    set_tests_properties(cli_music_fade_bad PROPERTIES PASS_REGULAR_EXPRESSION
        "--fade-in wants a non-negative number of seconds")
    gm_label(cli_music_fade_bad quick)
endif()

add_test(NAME cli_menu COMMAND gunmetal --menu)
set_tests_properties(cli_menu PROPERTIES PASS_REGULAR_EXPRESSION
    "MAIN MENU \\(id 1\\)")
gm_label(cli_menu quick)

add_test(NAME cli_menu_keys COMMAND gunmetal --menu-keys "down,down,enter")
set_tests_properties(cli_menu_keys PROPERTIES PASS_REGULAR_EXPRESSION
    "final: GRAPHICS OPTIONS \\(id 12\\)")
gm_label(cli_menu_keys quick)

add_test(NAME cli_menu_esc COMMAND gunmetal --menu-keys esc)
set_tests_properties(cli_menu_esc PROPERTIES PASS_REGULAR_EXPRESSION
    "QUIT GAME\\? \\(id 33\\)")
gm_label(cli_menu_esc quick)

add_test(NAME cli_menu_json COMMAND gunmetal --menu-json)
set_tests_properties(cli_menu_json PROPERTIES PASS_REGULAR_EXPRESSION
    "\"name\": \"MAIN MENU\"")
gm_label(cli_menu_json quick)

add_test(NAME cli_menu_bad COMMAND gunmetal --menu-keys bogus)
set_tests_properties(cli_menu_bad PROPERTIES PASS_REGULAR_EXPRESSION
    "\\[error\\] menu: unknown key")
gm_label(cli_menu_bad quick)

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
    # M6.5.1: `gunmetal --sim` is deterministic (same digest twice) and the
    # AI -> movement bridge actually moves things.
    add_test(NAME sim_check
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/sim_check.py
                     --level dm1 --ticks 500)
    set_tests_properties(sim_check PROPERTIES PASS_REGULAR_EXPRESSION
        "sim_check: ok")
    gm_label(sim_check quick)
    # A driven plan is deterministic too (the digest covers the player).
    add_test(NAME sim_drive_check
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/sim_check.py
                     --drive "forward=1:600;turn=0.5:200;fire:100" --ticks 1)
    set_tests_properties(sim_drive_check PROPERTIES PASS_REGULAR_EXPRESSION
        "sim_check: ok")
    gm_label(sim_drive_check quick)
    # The whole shipped corpus once: no level may crash the headless tick.
    add_test(NAME sim_corpus
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/sim_check.py
                     --all --ticks 100)
    set_tests_properties(sim_corpus PROPERTIES PASS_REGULAR_EXPRESSION
        "36 levels, 100 ticks, 0 failed")
    gm_label(sim_corpus full)
    # M6.5.1: the scripted player moves and fires through the proven
    # controller and weapon seams (`docs/SIM_PLAYER.md`).
    # M6.5.1: damage -> death (`armor <= 0`) -> respawn, and the input guard.
    add_test(NAME cli_sim_respawn
             COMMAND gunmetal --sim dm1 --sim-drive "fire:600" --sim-projectile
                     --sim-vitals 500,0 --sim-respawn 5)
    set_tests_properties(cli_sim_respawn PROPERTIES PASS_REGULAR_EXPRESSION
        "alive=1 damage_taken=1500 death_tick=[0-9]+ deaths=[1-9][0-9]*")
    gm_label(cli_sim_respawn quick)
    add_test(NAME cli_sim_vitals_bad
             COMMAND gunmetal --sim dm1 --sim-drive "fire:10" --sim-vitals 500)
    set_tests_properties(cli_sim_vitals_bad PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] sim: --sim-vitals wants ARMOR,SHIELD")
    gm_label(cli_sim_vitals_bad quick)
    add_test(NAME cli_sim_projectile
             COMMAND gunmetal --sim dm1 --sim-drive "fire:300" --sim-projectile)
    set_tests_properties(cli_sim_projectile PROPERTIES PASS_REGULAR_EXPRESSION
        "projectile=synthetic spawned=[1-9][0-9]* blasts=[1-9][0-9]*")
    gm_label(cli_sim_projectile quick)
    add_test(NAME cli_sim_drive
             COMMAND gunmetal --sim dm1 --sim-drive "fire:300")
    set_tests_properties(cli_sim_drive PROPERTIES PASS_REGULAR_EXPRESSION
        "shots=[1-9][0-9]* slot=0 ammo=")
    gm_label(cli_sim_drive quick)
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
    add_test(NAME lightmapedit_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/lightmapedit.py
                     --roundtrip)
    set_tests_properties(lightmapedit_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "round-trip: 51/51 identical")
    gm_label(lightmapedit_roundtrip quick)
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
    # M11.2: objects of the world (.sbw) - origin/basis/radius/bounds/flags.
    # M11.2 slice 4: vertices and the radius/bounds recompute rule (.sbw).
    add_test(NAME sbwmesh_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwmesh.py
                     --self-test)
    set_tests_properties(sbwmesh_selftest PROPERTIES PASS_REGULAR_EXPRESSION
        "81/81 passed")
    gm_label(sbwmesh_selftest quick)
    add_test(NAME sbwmesh_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwmesh.py
                     --roundtrip)
    set_tests_properties(sbwmesh_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "round-trip: 36/36 identical")
    gm_label(sbwmesh_roundtrip quick)
    add_test(NAME sbwmesh_audit
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwmesh.py
                     --audit)
    set_tests_properties(sbwmesh_audit PROPERTIES PASS_REGULAR_EXPRESSION
        "result: OK")
    gm_label(sbwmesh_audit quick)
    add_test(NAME sbwobj_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwobj.py
                     --self-test)
    set_tests_properties(sbwobj_selftest PROPERTIES PASS_REGULAR_EXPRESSION
        "82/82 passed")
    gm_label(sbwobj_selftest quick)
    add_test(NAME sbwobj_roundtrip
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwobj.py
                     --roundtrip)
    set_tests_properties(sbwobj_roundtrip PROPERTIES PASS_REGULAR_EXPRESSION
        "round-trip: 36/36 identical")
    gm_label(sbwobj_roundtrip quick)
    add_test(NAME sbwobj_audit
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbwobj.py
                     --audit)
    set_tests_properties(sbwobj_audit PROPERTIES PASS_REGULAR_EXPRESSION
        "result: OK")
    gm_label(sbwobj_audit quick)
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

# M7.4 slice 2: `--sample-play` decodes through SampleCache, not a bare lookup.
# `updown` is slot 0 of level0 (6272 frames, 16000 Hz, 12800 accounted bytes).
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/sounds/SOU.IDX"
   AND EXISTS "/mnt/d/games/Gunmetal 3dfx/level0.sbt")
    add_test(NAME cli_sample_play
             COMMAND gunmetal --sample-play updown --headless)
    set_tests_properties(cli_sample_play PROPERTIES PASS_REGULAR_EXPRESSION
        "sample: name=updown frames=[0-9]+ rate=[0-9]+ channels=1 bytes=[0-9]+.*misses=1")
    gm_label(cli_sample_play quick)
    # A second acquire in the same process must be a cache hit, not a decode.
    add_test(NAME cli_sample_play_twice
             COMMAND gunmetal --sample-play updown --headless --twice)
    set_tests_properties(cli_sample_play_twice PROPERTIES PASS_REGULAR_EXPRESSION
        "stats: hits=1 misses=1")
    gm_label(cli_sample_play_twice quick)
    add_test(NAME cli_sample_play_bad
             COMMAND gunmetal --sample-play nosuchname --headless)
    set_tests_properties(cli_sample_play_bad PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] sample-play: .*SOUND_SAMPLE_TABLE")
    gm_label(cli_sample_play_bad quick)
    # M7.3 slice 2: the positional path (`--pos`/`--listener`).  The numbers are
    # docs/AUDIO_3D.md §8.1: d = (48, 8, -184), dist 198, gain 58, pan 159,
    # L/R 23/32 (volume 64, atten 0.03, yaw 0).
    add_test(NAME cli_sample_play_3d
             COMMAND gunmetal --sample-play updown --headless --pos 48,3208,-136
                     --listener 0,3200,48,0)
    set_tests_properties(cli_sample_play_3d PROPERTIES PASS_REGULAR_EXPRESSION
        "dist=198\\.000000 gain=58 pan=159 L=23 R=32")
    gm_label(cli_sample_play_3d quick)
    # Same source with a +90° listener yaw (§8.2): only `e1` changes.
    add_test(NAME cli_sample_play_3d_yaw
             COMMAND gunmetal --sample-play updown --headless --pos 48,3208,-136
                     --listener 0,3200,48,1.5707963267948966)
    set_tests_properties(cli_sample_play_3d_yaw PROPERTIES PASS_REGULAR_EXPRESSION
        "dist=198\\.000000 gain=58 pan=122 L=32 R=29")
    gm_label(cli_sample_play_3d_yaw quick)
    # gain < 2 means no request (0x40be58): a far source is silence, not a
    # negative gain and not a quiet centre pan.
    add_test(NAME cli_sample_play_3d_silent
             COMMAND gunmetal --sample-play updown --headless --pos 100000,0,0)
    set_tests_properties(cli_sample_play_3d_silent PROPERTIES PASS_REGULAR_EXPRESSION
        "dist=100000\\.000000 gain=0 pan=255 L=0 R=0")
    gm_label(cli_sample_play_3d_silent quick)
    # Out-of-range options are a usage error (text + exit code 2), not silence.
    add_test(NAME cli_sample_play_3d_bad_volume
             COMMAND gunmetal --sample-play updown --headless --volume 40)
    set_tests_properties(cli_sample_play_3d_bad_volume PROPERTIES
        PASS_REGULAR_EXPRESSION "gunmetal: --volume expects an integer 1\\.\\.32")
    gm_label(cli_sample_play_3d_bad_volume quick)
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

# Broad-phase grid over the static triangles (docs/COLLIDE_GRID.md):
# `CollideGrid::raycast` must equal `CollisionWorld::raycast` bit for bit on 500
# deterministic rays of level0/dm1, under all 16 filter combinations and a
# `sectors` restriction; the candidate count must be well below a full scan;
# a repeated query must not change the grid; empty world / bad cell size /
# too-fine grid are refusals with an error line.
add_executable(gm_collide_grid_tests test_collide_grid.cpp)
target_link_libraries(gm_collide_grid_tests PRIVATE gm_engine)
add_test(NAME collide_grid COMMAND gm_collide_grid_tests)
gm_label(collide_grid quick)

# Volume queries over the static triangles (docs/COLLIDE_OVERLAP.md):
# `overlap_triangles` / `overlaps_static` (13-axis triangle/box SAT) and
# `overlap_sphere` (exact point-to-triangle distance) must agree with an
# independent brute-force reference (vertices/corners/edges, not SAT) on 240
# deterministic boxes and spheres per level; whole-world, void, point and
# filtered queries print their counts; min>max, NaN/inf, negative radius and an
# empty world are refusals with an error line.
add_executable(gm_collide_overlap_tests test_collide_overlap.cpp)
target_link_libraries(gm_collide_overlap_tests PRIVATE gm_engine)
add_test(NAME collide_overlap COMMAND gm_collide_overlap_tests)
gm_label(collide_overlap quick)

# M6.2.5 slice 2: target-aware line of sight over the level geometry
# (docs/LINE_OF_SIGHT.md).  Synthetic cube/shell pairs prove the strict
# `distance - radius` rule and that a target's own geometry is not cast
# against; real level0/dm1 pairs prove open space, walls, filters and the
# batch/single equivalence; NaN/inf/negative radius/empty world are refusals.
add_executable(gm_line_of_sight_tests test_line_of_sight.cpp)
target_link_libraries(gm_line_of_sight_tests PRIVATE gm_engine)
add_test(NAME line_of_sight COMMAND gm_line_of_sight_tests)
gm_label(line_of_sight quick)

# M6.4.1 slice 1: door leaves and lift cabins as kinematic colliders
# (docs/MOVING_COLLIDERS.md).  Real level0/dm1 door panels: a closed leaf holds
# a ray/sphere, an open one lets them through, travel comes from the door model
# (`open - closed`), normals/plane/centroid/bounds follow the moved vertices,
# filters pick by `surface_of`, NaN/inf are refused, a swing door is refused by
# `DoorAxis::Rotating`, and the ray/sphere sweeps are checked against
# `CollisionWorld` on the same triangles.
add_executable(gm_moving_collider_tests test_moving_collider.cpp)
target_link_libraries(gm_moving_collider_tests PRIVATE gm_engine)
add_test(NAME moving_collider COMMAND gm_moving_collider_tests)
gm_label(moving_collider quick)

# M6.4.1 slice 3: swing doors rotate (`0x44dfc0` lerp + `0x434887` basis,
# `world/moving_collider.h`).  Real level0 `sec=48 obj=6` (120 deg about -x)
# and dm1 `sec=17 obj=55` (-90 deg about -z): the closed pose is the rest pose
# bit for bit, half-open halves the angle and keeps edge lengths/planes, open
# clears the ray/sphere, the player is held by the closed leaf and walks through
# the open one, the runtime publishes the pair (`rotating`, suppressed static,
# posed dynamic), and zero angle / non-unit axis / NaN fraction are refused.
add_executable(gm_swing_door_tests test_swing_door.cpp)
target_link_libraries(gm_swing_door_tests PRIVATE gm_engine)
add_test(NAME swing_door COMMAND gm_swing_door_tests)
gm_label(swing_door quick)

# M6.4.1 slice 4: lift cabins (docs/LIFT_COLLISION.md).  The shipped data has
# no cabin -> `.sbw` binding (45/45 lifts unbound, probe in the doc), so the
# test pins the `lift_offset` seam on the real level13 sec=20 obj=29 plate, a
# caller-supplied `Lift::cabin_sector/cabin_object` binding (rest pose bit for
# bit, re-posed by `pos_delta * height/65535`, published only on a height
# change, static pair suppressed), and the negative: an unbound level ticks
# without touching the dynamic set (`lifts_unbound`).
add_executable(gm_lift_collision_tests test_lift_collision.cpp)
target_link_libraries(gm_lift_collision_tests PRIVATE gm_engine)
add_test(NAME lift_collision COMMAND gm_lift_collision_tests)
gm_label(lift_collision quick)

# M6.4.1 slice 2: moving doors really hold the player (docs/DOOR_COLLISION.md).
# `CollisionWorld::set_dynamic` + static suppression (NaN/inf refusal, empty-set
# parity), the runtime building one panel per moving sliding door on real
# level0/dm1 and rebuilding only when a door phase changed, a ray/sphere on the
# real level0 leaf closed vs open, `gm::step` stopped by the closed leaf and
# passing the open one, a SimPlayer walking through the real dm1 doorway, and
# the negatives (swing doors stay static, a door rebound to a missing object).
add_executable(gm_door_collision_tests test_door_collision.cpp)
target_link_libraries(gm_door_collision_tests PRIVATE gm_engine)
add_test(NAME door_collision COMMAND gm_door_collision_tests)
gm_label(door_collision quick)

# M6.4.1 slice 5: a moving panel carries the player standing on it
# (docs/PLATFORM_CARRY.md).  The opt-in `SimPlayer::set_platform_carry()` rule
# moves the capsule by the panel's own delta before the controller step (real
# level14 KELLIFT stand-in `sec=35 obj=1`, raise 260 over 30 ticks -> the player
# rises by 260, lower -> back down), forgets the panel on a step off (the fall
# takes over), cancels the transfer when the moved capsule would overlap a
# static wall, and leaves the slice-4 numbers verbatim when the rule is off
# (`dz = -39.306847`).
add_executable(gm_platform_carry_tests test_platform_carry.cpp)
target_link_libraries(gm_platform_carry_tests PRIVATE gm_engine)
add_test(NAME platform_carry COMMAND gm_platform_carry_tests)
gm_label(platform_carry quick)

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

# M6.1.6: a standalone camera model (docs/CAMERA_MODEL.md): the proven cockpit
# pose, the MODEL chase/orbit/fixed placements, the engine focal scales
# (1, 5/6 at 320x200) and focal_to_fov, the proven SET_CAMERA_*_SHAKE triangle
# table, a seeded xorshift32 shake impulse with an exact zero after 64 ticks,
# two-run bit equality, and text negatives for fov/near/far/dt/focal args.
add_executable(gm_camera_model_tests test_camera_model.cpp)
target_link_libraries(gm_camera_model_tests PRIVATE gm_engine)
add_test(NAME camera_model COMMAND gm_camera_model_tests)
gm_label(camera_model quick)

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

# M6.5.4: independent census of the whole installation (docs/WORLD_CENSUS.md).
# Loads all 36 `.sbt`/`.sbw` pairs with the C++ readers, prints one row per
# level and pins the ROADMAP numbers `levels=36 objects=28192 polygons=235069
# vertices=357818 doors=1779 switches=3964 lifts=45`; a second full pass must
# reproduce every row (determinism) and the per-level table must add up to the
# headline.  `full` only: it reads the whole corpus twice (~20 s).
add_executable(gm_world_census_tests test_world_census.cpp)
target_link_libraries(gm_world_census_tests PRIVATE gm_engine)
add_test(NAME world_census COMMAND gm_world_census_tests)
set_tests_properties(world_census PROPERTIES PASS_REGULAR_EXPRESSION
    "levels=36 objects=28192 polygons=235069 vertices=357818 doors=1779 switches=3964 lifts=45")
gm_label(world_census full)

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

# Music track (docs/MUSIC.md): linear fades and a linear-amplitude crossfade
# over a decoded PcmClip.  Synthetic tones pin gain(k) = k/N, the crossfade
# weights/peak/RMS and the negatives; real MUSIC/SOU.IDX data covers the
# shipped `L01BONUS` (16000 header, played at 32000) and Dm1A.sog.
add_executable(gm_music_tests test_music.cpp)
target_link_libraries(gm_music_tests PRIVATE gm_engine)
add_test(NAME music COMMAND gm_music_tests)
gm_label(music quick)

# M7.5 slice 2: `MUSIC/*.sog` playlists (audio/playlist.h, docs/PLAYLIST.md).
# The line model and every refusal first (no game needed), then the real
# installation: 651 files (598 `.sog` + 53 `.SOG`), five playlists pinned with
# their resolved index/size, the corpus aggregate (5211 names, 5088 found,
# 123 misses, 95 distinct absent names, 998 duplicates, Σ bytes) and the
# case-sensitive count proving the ASCII fold.  Skips the corpus half cleanly
# when the game is not installed.
add_executable(gm_playlist_tests test_playlist.cpp)
target_link_libraries(gm_playlist_tests PRIVATE gm_engine)
add_test(NAME playlist COMMAND gm_playlist_tests)
set_tests_properties(playlist PROPERTIES PASS_REGULAR_EXPRESSION
    "playlist: [0-9]+ checks, ok")
gm_label(playlist quick)

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

# M8.1 slice 2: the runtime cursor of the SML pager over `text_layout`
# (docs/SML_PAGER.md).  Page counts/lines/images are compared with an
# independent `layout_text` call on six shipped documents; forward/backward
# paging, first/last/show_page, the line cursor and the boundary negatives.
# Skips cleanly when the game is not installed.
add_executable(gm_sml_pager_tests test_sml_pager.cpp)
target_link_libraries(gm_sml_pager_tests PRIVATE gm_engine)
target_compile_definitions(gm_sml_pager_tests PRIVATE
    GM_PAGER_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/test_sml_pager.cpp")
add_test(NAME sml_pager COMMAND gm_sml_pager_tests)
gm_label(sml_pager quick)

# M8.1 slice 3: the current page painted into the 320x200 RGBA canvas
# (docs/PAGER_RENDER.md).  help.sme and CREDITS.SML: ink>0 and < 320*200, the
# stats against an independent walk over the page's runs, two byte-identical
# renders, different pages differ, the game-directory overload paints the same
# bytes, an empty document renders 0 ink, `--page 99` is refused and a closed
# pager / a missing atlas / a missing provider fail with a text.
# Skips cleanly when the game is not installed.
add_executable(gm_pager_render_tests test_pager_render.cpp)
target_link_libraries(gm_pager_render_tests PRIVATE gm_engine)
add_test(NAME pager_render COMMAND gm_pager_render_tests)
gm_label(pager_render quick)

# M8.3 slice 1: the briefing index (`ui/briefing`, docs/BRIEFING.md).  All 25
# `SML/brief/*.sme` stems, page/line/glyph numbers for every level, the text
# against gm/sml.py's whitespace-stripped `describe()["body"]` (FNV pasted from
# Python, never executed here), glyphs against `pager_render` on the same page,
# reload determinism and the text refusals (missing game/level/file, a path, a
# non-.sme name, empty/undecodable bytes, a directory the size of a file).
# Skips the corpus half cleanly when the game is not installed.
add_executable(gm_briefing_tests test_briefing.cpp)
target_link_libraries(gm_briefing_tests PRIVATE gm_engine)
add_test(NAME briefing COMMAND gm_briefing_tests)
gm_label(briefing quick)

# M8.3 slice 3: one briefing page painted into the 320x200 RGBA canvas
# (docs/BRIEFING_RENDER.md).  level1 page 1 pins pages/lines/glyphs/ink, the
# same frame through `SmlPager` + `render_page` is byte-identical (0 differing
# pixels), level0a pages 1/2/3 differ, --background replaces the black canvas
# with 64000 HELPSCRN pixels, all 25 briefings sum to 58 pages / 727 lines /
# 33284 glyphs with missing=0, and the refusals (level0, page 0/99, no
# directory, no BMP.IDX, broken .sme) are texts with a cleared buffer.
# Skips the corpus half cleanly when the game is not installed.
add_executable(gm_briefing_render_tests test_briefing_render.cpp)
target_link_libraries(gm_briefing_render_tests PRIVATE gm_engine)
add_test(NAME briefing_render COMMAND gm_briefing_render_tests)
gm_label(briefing_render quick)

# M8.2 slice 2: the menu screen painted into the 320x200 RGBA canvas
# (docs/MENU_RENDER.md).  MAIN / CONTROL OPTIONS / QUIT: items == items(),
# ink>0, background>0, missing=0, two byte-identical renders, a Down move
# changes the picture, PLATE (196x131) is the panel of screen 10 only, the
# game-directory overload paints the same bytes, None/Exit/no-provider/
# no-game-dir are refused with a text.  Skips the font runs cleanly when the
# game is not installed.
add_executable(gm_menu_render_tests test_menu_render.cpp)
target_link_libraries(gm_menu_render_tests PRIVATE gm_engine)
add_test(NAME menu_render COMMAND gm_menu_render_tests)
gm_label(menu_render quick)

# M8.4 slice 3: screen 17 `SELECT LEVEL FOR NEW GAME` painted into the 320x200
# RGBA canvas (docs/LEVEL_SELECT_RENDER.md).  counter 1: rows=7, title centred
# at y 14, row 0 at (14,27), ESC at (38,111), rows 7..9 empty, every band
# against an independent blit and the FONTS4 colours; counter 25: rows=10 and
# the scroll rule of 0x48a3f1 at 19/24/35 verified band by band; the cursor
# move, title/cancel switches, PLATE as an independently decoded background, a
# lowercase LVLNAMES.SLU in a temp tree (FRONTEND symlinked) and the refusals.
# Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_level_select_render_tests test_level_select_render.cpp)
target_link_libraries(gm_level_select_render_tests PRIVATE gm_engine)
add_test(NAME level_select_render COMMAND gm_level_select_render_tests)
gm_label(level_select_render quick)

# M8.5 slice 2: integer nearest upscale + palette fade of the 320x200 front-end
# canvas (docs/UPSCALE.md).  No game files: a 3x2 grid with unique colours at
# scale 1/2/3/8 checked point by point, the real 320x200 canvas at 1/2/4/8,
# fade level 0 (black, alpha kept) / max (byte identity) / clamp, monotonicity,
# determinism, the 256-entry tables against an independent Python reference,
# and the error contract (scale 0/9, null/empty source, size_t overflow,
# max_level < 1).
add_executable(gm_upscale_tests test_upscale.cpp)
target_link_libraries(gm_upscale_tests PRIVATE gm_engine)
add_test(NAME upscale COMMAND gm_upscale_tests)
gm_label(upscale quick)

# M8.5 slice 3: the exact fade tables of the cockpit band at 0x682f90
# (docs/HUD_LUT.md).  The 4096-byte contract (missing/empty/4095/4097 refused,
# a good file positional, stats, map clamp 0..15, describe 16 lines, the
# (v*k + max/2)/max model), then the real ~/.cache/gunmetal-re/hud_lut.bin:
# 16 tables, level 11 identity, levels 12..15 = 132/73/48/45 colour identities,
# and the per-level agreement with the RGBA model.  Skips the real half with a
# text when the dump or GAMEPAL is missing.
add_executable(gm_hud_lut_tests test_hud_lut.cpp)
target_link_libraries(gm_hud_lut_tests PRIVATE gm_engine)
add_test(NAME hud_lut COMMAND gm_hud_lut_tests)
gm_label(hud_lut quick)

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

# M8 slice 1: the headless front-end menu automaton (docs/FRONTEND.md): 34 §2
# screens, the 35-row §4 transition table with a comment/site per row, BFS
# reachability, Escape/back(), the QUIT path, determinism and the negatives.
# Pure state machine: no game installation is read, so it always runs.
add_executable(gm_frontend_menu_tests test_frontend_menu.cpp)
target_link_libraries(gm_frontend_menu_tests PRIVATE gm_engine)
add_test(NAME frontend_menu COMMAND gm_frontend_menu_tests)
gm_label(frontend_menu quick)

# M8.6 slice 2: menu event -> UI sample (docs/UI_SOUNDS.md §1.1/§1.2, §2.3,
# §2.4).  The six documented `sounds/SOU.IDX` record indices are checked
# against the real `SOUND_SAMPLE_TABLE` (`--level`, default level0; count > 0,
# slot name) and the archive order; the trigger table is checked key by key,
# including the model `menuon` edge and the silent Esc / KEY JAMMING cases.
add_executable(gm_ui_sounds_tests test_ui_sounds.cpp)
target_link_libraries(gm_ui_sounds_tests PRIVATE gm_engine)
add_test(NAME ui_sounds COMMAND gm_ui_sounds_tests)
gm_label(ui_sounds quick)

# M8.7 slice 1: the headless save-slot list (docs/SAVE_SLOTS.md, docs/UI_SCREENS.md
# §5.1): 20 table entries, the 0x4bc3b0 placeholder, 6 visible labels, the 0..13
# scroll, the selection crossing the window edge and the overwrite-confirmation
# model.  Synthetic saves come from the engine writer, so no install is needed;
# the shipped SAVEGAME is checked when present.
add_executable(gm_save_slots_tests test_save_slots.cpp)
target_link_libraries(gm_save_slots_tests PRIVATE gm_engine)
add_test(NAME save_slots COMMAND gm_save_slots_tests)
set_tests_properties(save_slots PROPERTIES PASS_REGULAR_EXPRESSION
    "save_slots: [0-9]+ checks, 20 slots, 6 visible, scroll 0\\.\\.13")
gm_label(save_slots quick)

# M8.4 slice 2: the save/load screen model on top of the slot list
# (docs/SAVE_MENU.md, docs/UI_SCREENS.md §5).  Save vs load mode, the 6-row
# window with the unreachable slot 19, name/level/seconds per row from the save
# header, the load/save/back event of Enter/Esc, and the refusals (empty slot on
# load, corrupt file, missing SAVEGAME) with texts.  Synthetic saves come from
# the engine writer; the shipped SAVEGAME is checked when present.
add_executable(gm_save_menu_tests test_save_menu.cpp)
target_link_libraries(gm_save_menu_tests PRIVATE gm_engine)
add_test(NAME save_menu COMMAND gm_save_menu_tests)
set_tests_properties(save_menu PROPERTIES PASS_REGULAR_EXPRESSION
    "save_menu: [0-9]+ checks, ok \\(20 slots, 6 visible, slot 19 unreachable\\)")
gm_label(save_menu quick)

# M8.7 slice 3: one frame of screen 14, SAVE GAME (docs/SAVE_MENU_RENDER.md,
# docs/UI_SCREENS.md §3.6).  Six slot bands at (42, 28 + 12*i), the cursor in
# font 4, `ESC TO CANCEL` at (38, 101), the 0..13 window of the model and the
# `::: empty slot :::` rows; the shipped saved0/saved1 give the real titles.
# Every band is compared with an independent blit; the window at selections
# 5/8/18, the cursor, a title blanked in a temp copy, PLATE as an independently
# decoded background and the refusals are pinned.  Skips the pixel runs cleanly
# when the game is not installed.
add_executable(gm_save_menu_render_tests test_save_menu_render.cpp)
target_link_libraries(gm_save_menu_render_tests PRIVATE gm_engine)
add_test(NAME save_menu_render COMMAND gm_save_menu_render_tests)
set_tests_properties(save_menu_render PROPERTIES PASS_REGULAR_EXPRESSION
    "save_menu_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(save_menu_render quick)

# M8.7 slice 5: one frame of screen 13, START GAME (docs/START_GAME_RENDER.md,
# docs/UI_SCREENS.md §3.6).  Title centred at y 12, the two entries at
# (35, 23) and (35, 33), six slot bands at (39, 44 + 11*i), the cursor in font 4,
# `ESC TO CANCEL` at (35, 110), the six-row window over the 20-entry table and
# the `MENUARRW` arrows at column 159 rows 2/121.  Every band is compared with an
# independent blit; `first_slot` 0/5/12, the cursor, a title blanked in a temp
# copy, PLATE as an independently decoded background and the refusals are
# pinned.  Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_start_game_render_tests test_start_game_render.cpp)
target_link_libraries(gm_start_game_render_tests PRIVATE gm_engine)
add_test(NAME start_game_render COMMAND gm_start_game_render_tests)
set_tests_properties(start_game_render PROPERTIES PASS_REGULAR_EXPRESSION
    "start_game_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(start_game_render quick)

# M8.8 slice 1: one frame of screen 19, MULTIPLAYER GAME TYPE
# (docs/GAME_TYPE_RENDER.md, docs/UI_SCREENS.md §3.8).  Title font 5 centred at
# y 10, the eleven modes at (18, 18 + 8*k) filtered by the byte mask 0x4c1290,
# `Cancel` last, the cursor (an item id) in font 4 and PLATE as an independently
# decoded background.  Every band is compared with an independent blit; the
# single-bit masks, the cursor 0/1/10/11, mask 0/0x100 and the refusals are
# pinned.  Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_game_type_render_tests test_game_type_render.cpp)
target_link_libraries(gm_game_type_render_tests PRIVATE gm_engine)
add_test(NAME game_type_render COMMAND gm_game_type_render_tests)
set_tests_properties(game_type_render PROPERTIES PASS_REGULAR_EXPRESSION
    "game_type_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(game_type_render quick)

# M8.8 slice 2: one frame of screen 23, `SET DIFFICULTY LEVEL` (`M8.8`,
# docs/DIFFICULTY_RENDER.md, docs/UI_SCREENS.md §2).  title centred at y 14,
# six ranks at (40, 27 + 15*i), the cursor row in font 4, `Cancel` as the last
# row (102), no ESC line and no MENUARRW, PLATE as an independently decoded
# background, the exact fade levels and the refusals.  Every band is compared
# with an independent blit; bboxes and diff counts are printed.  Skips the pixel
# runs cleanly when the game is not installed.
add_executable(gm_difficulty_render_tests test_difficulty_render.cpp)
target_link_libraries(gm_difficulty_render_tests PRIVATE gm_engine)
add_test(NAME difficulty_render COMMAND gm_difficulty_render_tests)
set_tests_properties(difficulty_render PROPERTIES PASS_REGULAR_EXPRESSION
    "difficulty_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(difficulty_render quick)

# M8.8 slice 3: one frame of screen 21, `NUMBER OF LOCAL PLAYERS`
# (docs/LOCAL_PLAYERS_RENDER.md, docs/UI_SCREENS.md §3.8).  title centred at
# y 14, two text lines per item at (38, 27) / (38, 36) with item step 27, so
# item 0 y 27/36, item 1 y 54/63, `Cancel` y 81; both lines of the cursor item
# switch to font 4, no MENUARRW, PLATE as an independently decoded background,
# the exact fade levels and the refusals.  Every band is compared with an
# independent blit; bboxes and diff counts are printed.  Skips the pixel runs
# cleanly when the game is not installed.
add_executable(gm_local_players_render_tests test_local_players_render.cpp)
target_link_libraries(gm_local_players_render_tests PRIVATE gm_engine)
add_test(NAME local_players_render COMMAND gm_local_players_render_tests)
set_tests_properties(local_players_render PROPERTIES PASS_REGULAR_EXPRESSION
    "local_players_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(local_players_render quick)
# M8.8 slice 4: one frame of screen 33, `QUIT GAME?` / `QUIT OPTIONS`
# (docs/QUIT_RENDER.md, docs/UI_SCREENS.md §2/§3.8/§12.4).  Two variants of the
# same handler `0x488780`: outside the game `QUIT GAME?` with `Yes`/`No`
# (rows 2), in game `QUIT OPTIONS` with `Quit Game`/`Quit To Title`/
# `Return To Game` (rows 3); title font 5 centred at y 14, items at (38,
# 27 + 16*i), the cursor item in font 4, no MENUARRW, PLATE as an independently
# decoded background, the exact fade levels and the refusals.  Every band is
# compared with an independent blit; bboxes and diff counts are printed.  Skips
# the pixel runs cleanly when the game is not installed.
add_executable(gm_quit_render_tests test_quit_render.cpp)
target_link_libraries(gm_quit_render_tests PRIVATE gm_engine)
add_test(NAME quit_render COMMAND gm_quit_render_tests)
set_tests_properties(quit_render PROPERTIES PASS_REGULAR_EXPRESSION
    "quit_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(quit_render quick)

# M8.8 slice 5: one frame of screen 37, `NEW VIDEO MODE OK?`
# (docs/VIDEO_MODE_RENDER.md, docs/UI_SCREENS.md §3.8).  title centred at
# y 14, `Yes` (item 0) at (38, 27) and `No` (item 1) at (38, 43) because both
# calls pass row 27 with item step 16; the cursor item switches to font 4, no
# MENUARRW, PLATE as an independently decoded background, the exact fade levels
# and the refusals.  Every band is compared with an independent blit; bboxes
# and diff counts are printed.  Skips the pixel runs cleanly when the game is
# not installed.
add_executable(gm_video_mode_render_tests test_video_mode_render.cpp)
target_link_libraries(gm_video_mode_render_tests PRIVATE gm_engine)
add_test(NAME video_mode_render COMMAND gm_video_mode_render_tests)
set_tests_properties(video_mode_render PROPERTIES PASS_REGULAR_EXPRESSION
    "video_mode_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(video_mode_render quick)

# M8.8 slice 7: one frame of screen 18, `SELECT VIDEO MODE`
# (docs/VIDEO_MODE_LIST_RENDER.md, docs/UI_SCREENS.md §2/§3.8).  Title centred
# at y 11, `Press Esc to close` centred at y 20, ten bands at (38, 29 + 9*i),
# the cursor band in font 4, the `MENUARRW` up/down arrows at (159, 2)/(159,
# 121) of the ten-band window, PLATE as an independently decoded background,
# the exact fade levels and the refusals.  The mode list itself is the
# documented model of the header (`kVideoModeListModes`), not the driver's
# answer; the test pins its labels, the window rule, the arrow pixels and the
# bboxes.  Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_video_mode_list_render_tests test_video_mode_list_render.cpp)
target_link_libraries(gm_video_mode_list_render_tests PRIVATE gm_engine)
add_test(NAME video_mode_list_render COMMAND gm_video_mode_list_render_tests)
set_tests_properties(video_mode_list_render PROPERTIES PASS_REGULAR_EXPRESSION
    "video_mode_list_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(video_mode_list_render quick)

# M8.8 slice 6: one frame of screen 6, `RESTART GAME` (docs/RESTART_RENDER.md,
# docs/UI_SCREENS.md §2/§3.8, docs/GAME_OVER.md §4.1).  A runtime row count
# (0x67f10c written last from `esi`): `items` 2..4 are the gated
# Reload/R.P.V./HQ prefix plus the unconditional `Quit to title`, rows at
# (10, 27 + 16*i), the cursor row in font 4, no ESC line, no MENUARRW, PLATE as
# an independently decoded background, the exact fade levels and the refusals.
# `items` outside 2..4 is clamped (1 -> 2, 99 -> 4).  Every band is compared
# with an independent blit; bboxes and diff counts are printed.  Skips the pixel
# runs cleanly when the game is not installed.
add_executable(gm_restart_render_tests test_restart_render.cpp)
target_link_libraries(gm_restart_render_tests PRIVATE gm_engine)
add_test(NAME restart_render COMMAND gm_restart_render_tests)
set_tests_properties(restart_render PROPERTIES PASS_REGULAR_EXPRESSION
    "restart_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(restart_render quick)

# M8.8 slice 8: one frame of screen 22, `MOUSE SETUP` (docs/MOUSE_SETUP_RENDER.md,
# docs/UI_SCREENS.md §2/§3.5).  rows 12, step 8, base row 24 (x 18 for `Used By`,
# x 9 for the rest), title font 5 centred at y 11, three `SLIDE` (105, 40/56/72) +
# `KNOB` (108 + 2.7 px per tenth of 0.2..2.2) pairs, the cursor item in font 4,
# no MENUARRW, PLATE as an independently decoded background, the exact fade
# levels and the refusals.  Every band is compared with an independent blit; the
# knob shift, bboxes and diff counts are printed.  Skips the pixel runs cleanly
# when the game is not installed.
add_executable(gm_mouse_setup_render_tests test_mouse_setup_render.cpp)
target_link_libraries(gm_mouse_setup_render_tests PRIVATE gm_engine)
add_test(NAME mouse_setup_render COMMAND gm_mouse_setup_render_tests)
set_tests_properties(mouse_setup_render PROPERTIES PASS_REGULAR_EXPRESSION
    "mouse_setup_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(mouse_setup_render quick)

# M8.8 slice 9: one frame of screen 27, `MULTIPLAY GAME OPTIONS`
# (docs/MATCH_SETUP_RENDER.md, docs/UI_SCREENS.md §2/§3.8).  rows 8, step 16,
# title font 5 centred at y 11, the two value rows at x 23 (match type y 22,
# `Starting Cash: <value>` y 32) and the five option rows at x 35 (y
# 45/60/75/90/103), `START GAME!` at (23,113) in font 5 (font 4 when the cursor
# is 7), the five `CHECK`/`NOCHECK` boxes anchored at (24, text_y - 8), no
# `SLIDE`/`KNOB` and no `MENUARRW`, PLATE as an independently decoded
# background, the exact fade levels and the refusals.  Every band is compared
# with an independent blit; the checkbox toggle, bboxes and diff counts are
# printed.  Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_match_setup_render_tests test_match_setup_render.cpp)
target_link_libraries(gm_match_setup_render_tests PRIVATE gm_engine)
add_test(NAME match_setup_render COMMAND gm_match_setup_render_tests)
set_tests_properties(match_setup_render PROPERTIES PASS_REGULAR_EXPRESSION
    "match_setup_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(match_setup_render quick)

# M8.8 slice 11: one frame of screen 16, `KEY JAMMING` (docs/KEY_JAMMING_RENDER.md,
# docs/UI_SCREENS.md §2/§3.8).  rows 1, step 0, title font 5 centred at y 14, the
# three centred hints `Hold down keys to check` / `if they jam.` /
# `Press Esc to close.` at y 26/35/44, and the key-state readout of `0x4bf2a8`:
# up to 28 names of the 123-entry table `0x4a5b88` in four columns
# (x 30/64/98/132, y 54 + 8*row), font 0 (font 4 when the inherited cursor is
# 0), PLATE as an independently decoded background, the exact fade levels and
# the refusals.  Every band is compared with an independent blit; the key grid,
# the PLATE diff and the fade counts are printed.  Skips the pixel runs cleanly
# when the game is not installed.
add_executable(gm_key_jamming_render_tests test_key_jamming_render.cpp)
target_link_libraries(gm_key_jamming_render_tests PRIVATE gm_engine)
add_test(NAME key_jamming_render COMMAND gm_key_jamming_render_tests)
set_tests_properties(key_jamming_render PROPERTIES PASS_REGULAR_EXPRESSION
    "key_jamming_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(key_jamming_render quick)

# M8.8 slice 14: one frame of screen 20, `LAY DOWN THE BUCKS`
# (docs/LAY_BUCKS_RENDER.md, docs/UI_SCREENS.md §2).  rows 1, step 0, the title
# font 5 centred at y 14 and the three notices `THIS FEATURE IS NOT` /
# `AVAILABLE IN THE` / `SHAREWARE VERSION` at x 42, y 27/36/45 in font 0; all
# four calls pass item = -1, so there is no cursor, no highlight and no font 4,
# and the screen reads no amount (the `cash` option changes nothing).  PLATE as
# an independently decoded background, the exact fade levels and the refusals.
# Every band is compared with an independent blit; the bboxes, the cash diff and
# the PLATE/fade counts are printed.  Skips the pixel runs cleanly when the game
# is not installed.
add_executable(gm_lay_bucks_render_tests test_lay_bucks_render.cpp)
target_link_libraries(gm_lay_bucks_render_tests PRIVATE gm_engine)
add_test(NAME lay_bucks_render COMMAND gm_lay_bucks_render_tests)
set_tests_properties(lay_bucks_render PROPERTIES PASS_REGULAR_EXPRESSION
    "lay_bucks_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(lay_bucks_render quick)
# M8.8 slice 16: one frame of screen 5, `ERROR` (docs/ERROR_RENDER.md,
# docs/UI_SCREENS.md §2).  rows 0, step 0, the title font 5 centred at y 14, the
# word loop of the error buffer 0x4aa2f8 (font 0, x from 10, y from 34, wrap at
# 180 px, pitch 10, 6-px gap) and the prompt `ESC TO RETURN TO MENU` centred at
# y 91; every call passes item = -1, so there is no cursor and no font 4.  The
# default message is the shipped `No Error.`; the option changes the frame and
# an empty message reproduces the empty-buffer branch.  PLATE as an
# independently decoded background, the exact fade levels and the refusals.
# The whole canvas is compared with an independent composition of the model
# layout; the bboxes, the message diffs and the PLATE/fade counts are printed.
# Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_error_render_tests test_error_render.cpp)
target_link_libraries(gm_error_render_tests PRIVATE gm_engine)
add_test(NAME error_render COMMAND gm_error_render_tests)
set_tests_properties(error_render PROPERTIES PASS_REGULAR_EXPRESSION
    "error_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(error_render quick)

# M8.8 slice 13: one frame of screen 15, the save-name entry
# (docs/SAVE_NAME_RENDER.md, docs/UI_SCREENS.md §2/§3/§5.2).  The map has no
# handler for 15 because the input function 0x48b4d0 paints the screen itself:
# the state-15 branch (0x48b691) calls 0x48abd0 (the SAVE GAME frame: title font
# 5 centred at y 14 and `ESC TO CANCEL` at (38,101)), then the FILESLOT 124x10
# bar at (38, 27 + 12*row) and the input buffer 0x661280 at (42, 28 + 12*row) in
# font 4 (forced by 0x485e19).  The caret is the literal trailing '-' of
# 0x48b6d1 (no sprite, no blink); the buffer is capped at 18 bytes / 17
# characters (cmp ecx,0x12).  The test pins rows 1 / step 0, every band against
# an independent blit, the bar in the original file-row order, a 30-character
# name cut to 17 + caret, the caret pixels, the row clamp, PLATE as an
# independently decoded background, the exact fade levels and the refusals.
# Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_save_name_render_tests test_save_name_render.cpp)
target_link_libraries(gm_save_name_render_tests PRIVATE gm_engine)
add_test(NAME save_name_render COMMAND gm_save_name_render_tests)
set_tests_properties(save_name_render PROPERTIES PASS_REGULAR_EXPRESSION
    "save_name_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(save_name_render quick)

# M8.8 slice 12: one frame of screen 9, the in-game screen
# (docs/INGAME_MENU_RENDER.md, docs/UI_SCREENS.md §2/§3.8).  The screen has no
# menu: the id 9 slot of 0x4883d8 is the dispatcher's shared tail 0x487f64
# (PLATE + carousel), the state-9 branch 0x4876af returns at 0x487739 before
# the dispatcher, and no 0x485dd0 call exists in the level code.  The branch
# only loads one 320x200 picture (DEMOTION or DISBAND, chosen at 0x497210) into
# the canvas at (0,0) and presents it.  The test pins the asset layout (320x200,
# no key, engine row order = flipV, palette == GAMEPAL on the used indices), the
# three diff counts (DEMOTION vs DISBAND 49323, each vs black 64000), the empty
# menu model (rows/ink/missing/arrows 0, the cursor is a no-op), the exact fade
# levels and the refusals; a DEMOTION-only archive renders, which proves no
# font/plate is needed.  Skips the pixel runs cleanly when the game is not
# installed.
add_executable(gm_ingame_menu_render_tests test_ingame_menu_render.cpp)
target_link_libraries(gm_ingame_menu_render_tests PRIVATE gm_engine)
add_test(NAME ingame_menu_render COMMAND gm_ingame_menu_render_tests)
set_tests_properties(ingame_menu_render PROPERTIES PASS_REGULAR_EXPRESSION
    "ingame_menu_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(ingame_menu_render quick)

# M8.8 slice 15: one frame of screen 36, the per-player JOYSTICK SETUP
# (docs/PLAYER_SETUP_RENDER.md, docs/UI_SCREENS.md §2/§3.8).  The handler
# 0x489d60 (slot 0x488454 -> stub 0x487f51) draws the title
# `PLAYER 1/2 JOYSTICK SETUP` (font 5, centred, y 11), the hint
# `Esc Exit, Button To Highlight` (font 0, centred, y 20), then the device name,
# six axis rows (`Left/Right:` ... `Angular Z:`, preset names 0x4bc468), two hat
# pairs (`Hat 1/2 X/Y`) and all 32 button channels (mask 0x4aa490 -> 0x4863a0 ->
# action names 0x4bc558) in a ten-band window with the five-row scroll band and
# the two MENUARRW arrows at (159,2)/(159,121).  Highlight: font 4 on the cursor
# row, font 5 for a deflected axis (|v| >= 0.5), active hat bits and pressed
# buttons.  There are no SLIDE/KNOB and no CHECK/NOCHECK calls on this screen.
# PLATE as an independently decoded background, the exact fade levels and the
# refusals.  Every band is compared with an independent blit; the boxes, the
# inks, the value column and the scroll/arrow counts are printed.  Skips the
# pixel runs cleanly when the game is not installed.
add_executable(gm_player_setup_render_tests test_player_setup_render.cpp)
target_link_libraries(gm_player_setup_render_tests PRIVATE gm_engine)
add_test(NAME player_setup_render COMMAND gm_player_setup_render_tests)
set_tests_properties(player_setup_render PROPERTIES PASS_REGULAR_EXPRESSION
    "player_setup_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(player_setup_render quick)

# M8.8 slice 17: one frame of screen 10, CONTROL OPTIONS
# (ui/control_options_render.*, docs/CONTROL_OPTIONS_RENDER.md,
# docs/UI_SCREENS.md §2/§3.1).  Title `CONTROL OPTIONS` in font 5 centred at y 14;
# ten lines at y = 27 + 9*i, x 20 (and x 32 for the two Assign/View lines 1/3):
# `Player 1: %s` / `View|Assign %s`, the same pair for player 2, `Keyjamming`,
# `1: %.25s` / `Player N Joystick: Not Used`, `Joystick Centering`,
# `Mouse: %s` (0x4bc548) and `Done`; the cursor line in font 4.  Screen 10 has no
# SLIDE/KNOB, no CHECK/NOCHECK and no MENUARRW at all.  PLATE as an independently
# decoded background, the exact fade levels and the refusals; every band is
# compared with an independent blit and the boxes/inks are printed.  Skips the
# pixel runs cleanly when the game is not installed.
add_executable(gm_control_options_render_tests test_control_options_render.cpp)
target_link_libraries(gm_control_options_render_tests PRIVATE gm_engine)
add_test(NAME control_options_render COMMAND gm_control_options_render_tests)
set_tests_properties(control_options_render PROPERTIES PASS_REGULAR_EXPRESSION
    "control_options_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(control_options_render quick)

# M8.7 slice 7: one frame of screen 30, the KEY SET VIEW (docs/KEYCFG_RENDER.md,
# docs/UI_SCREENS.md §3.7).  Title = the profile's `name[16]` in font 5 centred
# at y 11, `Press Esc to close` at y 20, the 31-action list with step 8 and the
# ten-band window at y 29 (action x 5, key x 108, second code x 154), the down
# `MENUARRW` at (159, 121) and PLATE as an independently decoded background.
# Real `KEYCFG/PERFECT.key`/`SPLIT1.KEY`/`SPLIT2.KEY`; every band is compared
# with an independent blit; the name band, the profile diff, the window and the
# refusals (375/377 bytes, no BMP.IDX, no FONTS4, unknown background) are pinned.
# Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_keycfg_render_tests test_keycfg_render.cpp)
target_link_libraries(gm_keycfg_render_tests PRIVATE gm_engine)
add_test(NAME keycfg_render COMMAND gm_keycfg_render_tests)
set_tests_properties(keycfg_render PROPERTIES PASS_REGULAR_EXPRESSION
    "keycfg_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(keycfg_render quick)

# M8.7/M9.3 slice 2: the save/load action on top of the screen model
# (game/save_action.h, docs/SAVE_ACTION.md).  A `save` event writes
# `saved<N>.sbt` into a scratch directory through `write_save()` (byte for
# byte the shipped `saved0.sbt`, SHA-256 printed), an empty slot is refused
# instead of inventing a template, a `load` event applies the slot through
# `game/save_load.*` (LEVEL0, the saved0 pose/gauges/loadout), and a write into
# the game installation is refused with the install left byte-identical.
# Synthetic empty/corrupt directories need no installation; the shipped-save
# half is skipped cleanly when it is absent.
add_executable(gm_save_action_tests test_save_action.cpp)
target_link_libraries(gm_save_action_tests PRIVATE gm_engine)
add_test(NAME save_action COMMAND gm_save_action_tests)
set_tests_properties(save_action PROPERTIES PASS_REGULAR_EXPRESSION
    "save_action: [0-9]+ checks, ok")
gm_label(save_action quick)

# M8.4 slice 2: the best-time screen model on top of `GUNMETAL.HI`
# (ui/records.h, docs/HI_SCORES.md, docs/CAMPAIGN_FLOW.md §6,
# docs/GAME_OVER.md §5).  The shipped 576-byte / 9-row file read only: the
# times 146,1570,18,109,18,7,11,40,6, the two duplicate names, the minimum per
# name (TESTING RANGE 18, KELLER CORE 1570, NATAKA TOWER 7), the strict better()
# decision, an append on a scratch copy (+0x40, old bytes intact, 10 rows on
# re-read), the `0x483f00` time text and the size/name/time refusals.
add_executable(gm_records_tests test_records.cpp)
target_link_libraries(gm_records_tests PRIVATE gm_engine)
add_test(NAME records COMMAND gm_records_tests)
set_tests_properties(records PROPERTIES PASS_REGULAR_EXPRESSION
    "records: [0-9]+ checks, ok \\((rows=9 best=7 duplicates=2|shipped file not installed)\\)")
gm_label(records quick)

# M8.4 slice 3: the kill-message screen model on top of
# FRONTEND/KILLED.skb|.skk (ui/killed.h, docs/KILLED.md, docs/SUB.md §7).  The
# two shipped files read only: 2338/1967 bytes, 92/74 CRLF lines, the
# 0x485a7d filter keeping 69/50 messages, the `%S` substitution (138/50 of
# 182/73 placeholders), `pick` = seed % n, the 0x80 cap, the 30/31-byte
# boundary, CRLF/LF/NUL/high-byte behaviour and the text refusals (no file,
# empty file, no message, a 16-byte name, `%S` without a name).
add_executable(gm_killed_tests test_killed.cpp)
target_link_libraries(gm_killed_tests PRIVATE gm_engine)
add_test(NAME killed COMMAND gm_killed_tests)
set_tests_properties(killed PROPERTIES PASS_REGULAR_EXPRESSION
    "killed: [0-9]+ checks, ok \\((skb raw=92 kept=69 subs=138, skk raw=74 kept=50 subs=50|shipped files not installed)\\)")
gm_label(killed quick)

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

# M4.2.6 slice 2: negative index -> `.sbw` effect record -> frame texture ->
# PICS/sprites archive (render/fx_frame.h, docs/FX_FRAME.md).  Synthetic
# refusals plus level0/dm1 resolution and the 36-world totals of
# docs/EFFECTS.md §3 (6135 effect polygons, 262 pairs, 1274 frame references,
# 212 textures, 0 unresolved).
add_executable(gm_fx_frame_tests test_fx_frame.cpp)
target_link_libraries(gm_fx_frame_tests PRIVATE gm_engine)
add_test(NAME fx_frame COMMAND gm_fx_frame_tests)
gm_label(fx_frame quick)

# M4.2.2 slice 2: polygon texture scroll (`+0x5c`/`+0x60`, helper 0x434c80) and
# the animated 16-bit halves (`+0x5e`/`+0x62`, lerp 0x4342f0) as pure
# arithmetic (render/texture_anim.h, docs/TEXTURE_ANIM.md).  Synthetic cases
# pin the proven formula, the u16 lerp and every refusal; real level0/dm1 and
# all 36 worlds pin 2917 scrolled polygons (1.24 % of 235 069), 235 069
# non-zero anim slots, max |scroll_u| 15224 (dm2) and max |scroll_v| 10000
# (level17).
add_executable(gm_texture_anim_tests test_texture_anim.cpp)
target_link_libraries(gm_texture_anim_tests PRIVATE gm_engine)
add_test(NAME texture_anim COMMAND gm_texture_anim_tests)
gm_label(texture_anim quick)

# M6.3.1 slice 2: the "AI state -> clip -> frame" join for things
# (world/thing_anim.h, docs/THING_ANIM_BIND.md).  Synthetic cases pin the
# state map, the `_idle`/`_walk`/`_atk`/`_dead` probe, every refusal and the
# 91 Hz hold/loop split on hand-written clips; the real cases pin the shipped
# `Sobanim/` corpus (40 clips, 0 state-specific variants, 26 mode 0 / 14
# mode 1), a type from level13 (`CULTIST` -> `Cultist.soo`) and the mode 0
# (`kndeath`, 4x15) vs mode 1 (`kntbox`, 10x20) timelines.
add_executable(gm_thing_anim_tests test_thing_anim.cpp)
target_link_libraries(gm_thing_anim_tests PRIVATE gm_engine)
add_test(NAME thing_anim COMMAND gm_thing_anim_tests)
gm_label(thing_anim quick)

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

# M7.4: runtime sample cache (docs/SAMPLE_CACHE.md).  Ref-counted
# `SOUND_SAMPLE_TABLE` slots, real SOU0/SOU1/SOU2 decoding through
# `sounds/SOU.IDX`, LRU eviction under the `0x100 + PCM` byte budget, quiet
# missing/count-0 nullopts and deterministic stats.
add_executable(gm_sample_cache_tests test_sample_cache.cpp)
target_link_libraries(gm_sample_cache_tests PRIVATE gm_engine)
add_test(NAME sample_cache COMMAND gm_sample_cache_tests)
gm_label(sample_cache quick)

# M7.6 slice 2: missing-sample policy (docs/SOUND_POLICY.md).  `sounds` ->
# `MUSIC` fallback -> -1/-2 -> silence, with counters and one `Warn` per name;
# the docs/SOUND_NAMES.md numbers are pinned on level0/dm1.
add_executable(gm_sound_policy_tests test_sound_policy.cpp)
target_link_libraries(gm_sound_policy_tests PRIVATE gm_engine)
add_test(NAME sound_policy COMMAND gm_sound_policy_tests)
gm_label(sound_policy quick)

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

# Engine save writer (M8.7/M9.4, docs/SAVE_WRITE_CPP.md): `write_save()` over
# the same model.  Shipped saved0/saved1 parse -> write -> bytes byte for byte
# (size/SHA-256/FNV printed by the test), the opaque header bytes 0x10..0x50
# survive from the original, title/vitals edits touch only their own bytes, and
# an empty model, an out-of-range offset/size, a duplicate id, a missing player
# or an unrepresentable field are refused with a text and a code.
# `--emit DIR` writes the C++ bytes for the documented cmp against
# tools/re/save_rebuild.py.
add_executable(gm_save_write_tests test_save_write.cpp)
target_link_libraries(gm_save_write_tests PRIVATE gm_engine)
add_test(NAME save_write COMMAND gm_save_write_tests)
set_tests_properties(save_write PROPERTIES PASS_REGULAR_EXPRESSION
    "save_write: [0-9]+ checks, ok")
gm_label(save_write quick)

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

# Event-driven damage to things (M6.2.5, docs/EVENT_DAMAGE.md): the level-wide
# SCRIPT scan (`build_event_registry`) and the `damaged_event` chain
# (`chase_damaged_events`) that the runtime bridge runs after a blast.
add_executable(gm_event_damage_tests test_event_damage.cpp)
target_link_libraries(gm_event_damage_tests PRIVATE gm_engine)
add_test(NAME event_damage COMMAND gm_event_damage_tests)
gm_label(event_damage quick)

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
add_executable(gm_chassis_loadout_tests test_chassis_loadout.cpp)
target_link_libraries(gm_chassis_loadout_tests PRIVATE gm_engine)
# `DEFINE_PLAYER_TYPES` -> the nine fields the original applies (M6.1.4):
# the five shipped chassis, the *100 armour scaling, the 0x479dd0 shop price
# and the ChassisMotion mapping; real level0/dm1 branches; short/NaN/negative/
# fractional/out-of-range negatives.
add_test(NAME chassis_loadout COMMAND gm_chassis_loadout_tests)
gm_label(chassis_loadout quick)

# M6.1.1: standalone chassis physics (docs/CHASSIS_PHYSICS.md).  The eight
# doubles of the record (`+0x48`, op17..op24) verified on 616/616 records of
# 36 levels, the retention k (op20 = 0x81bfb8) of all five shipped chassis,
# the proven plateau `op4*91/(1-k)` and `-91*ln(k)` drag, the decayed and the
# undamped suspension, the loaded ride height, gravity free fall, mass
# invariance, 1000-step bitwise determinism and the refusals.
add_executable(gm_chassis_physics_tests test_chassis_physics.cpp)
target_link_libraries(gm_chassis_physics_tests PRIVATE gm_engine)
add_test(NAME chassis_physics COMMAND gm_chassis_physics_tests)
gm_label(chassis_physics quick)

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

# M6.5.1 slice 1: one scripted player over `GameRuntime` (docs/SIM_PLAYER.md).
# `parse_drive` positives/negatives, then on dm1: 900 ticks of forward from
# the campaign spawn of slot 0 (displacement, finite state), a two-run
# determinism check, a wall run aimed at the nearest geometry found with
# `CollisionWorld::raycast`, `on_ground` on a front-wound dm1 floor and a
# fire=300 segment that spends ammo; one smoke case on level1.
add_executable(gm_sim_player_tests test_sim_player.cpp)
target_link_libraries(gm_sim_player_tests PRIVATE gm_engine)
add_test(NAME sim_player COMMAND gm_sim_player_tests)
gm_label(sim_player quick)

# M6.5.1 slice 2: player -> projectile -> hit -> blast (docs/SIM_PROJECTILE.md).
# Real dm1 + level0 ROCKET + a movement-type 6 template; the weakest damageable
# thing with a firing line is found from the level itself, then `fire` must
# spawn a round, expire it on the wall, blast and kill the thing; replay and the
# no-template / non-projectile-template negatives.
add_executable(gm_sim_projectile_tests test_sim_projectile.cpp)
target_link_libraries(gm_sim_projectile_tests PRIVATE gm_engine)
add_test(NAME sim_projectile COMMAND gm_sim_projectile_tests)
gm_label(sim_projectile quick)

# M6.5.1 slice 3: player damage, death and respawn (docs/SIM_DAMAGE.md).
# Real dm1 + level0 ROCKET + the LIGHT starting gauges of saved0; point-blank
# self-damage, fire until armor 0 (shields first), the no-op tick after death,
# respawn() back to the starting feet/yaw and gauges, damage after respawn,
# custom gauges via set_vitals(), a determinism replay and the negatives.
add_executable(gm_sim_damage_tests test_sim_damage.cpp)
target_link_libraries(gm_sim_damage_tests PRIVATE gm_engine)
add_test(NAME sim_damage COMMAND gm_sim_damage_tests)
gm_label(sim_damage quick)

# M9.5 slice 1: golden deterministic `--sim` scenarios (docs/SIM_SCENARIOS.md).
# The `main.cpp::simulate()` drive loop repeated step for step on real dm1:
# forward/turn kinematics, a hitscan fire series, the synthetic `--sim-projectile`
# round from the spawn, an aimed round at the weakest damageable thing, death
# with `--sim-vitals 500,0` + `--sim-respawn 20`, and the `--god` VM flag (which
# does not guard the player gauges).  Every number is a golden and every
# scenario is replayed on a fresh runtime.
add_executable(gm_sim_scenario_tests test_sim_scenario.cpp)
target_link_libraries(gm_sim_scenario_tests PRIVATE gm_engine)
add_test(NAME sim_scenario COMMAND gm_sim_scenario_tests)
gm_label(sim_scenario quick)

# M8.7/M9 slice 2: save -> live runtime (docs/SAVE_LOAD.md).  Both shipped
# saves: the saved pose/gauges/loadout land in the SimPlayer *and* the
# GameRuntime, block 8 reaches VmState::sectors, the header level loads
# (58 sectors / 192 objects / 121 things), 60 ticks are deterministic, armor 0
# is a dead player, and the broken/missing/out-of-range inputs are refused.
add_executable(gm_save_load_tests test_save_load.cpp)
target_link_libraries(gm_save_load_tests PRIVATE gm_engine)
add_test(NAME save_load COMMAND gm_save_load_tests)
set_tests_properties(save_load PROPERTIES PASS_REGULAR_EXPRESSION
    "save_load: ([0-9]+ checks, ok|saved0/saved1/level0 not installed, skipped)")
gm_label(save_load quick)

# `--save-load 0`: the CLI chain on the shipped saved0 (numbers are the
# module test's), plus the tick run and the refused slot.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx/SAVEGAME/saved0.sbt")
    add_test(NAME cli_save_load
             COMMAND gunmetal --save-load 0 --save-load-ticks 0)
    set_tests_properties(cli_save_load PROPERTIES PASS_REGULAR_EXPRESSION
        "save-load: slot=0 level=level0 pos=\\(1116\\.174, 798\\.208, 48\\.000\\) armor=10000 shields=8000/8000/8000/8000 ammo=2000,90 sector_stuff=13 unsupported=15 ticks=0")
    gm_label(cli_save_load quick)
    add_test(NAME cli_save_load_ticks
             COMMAND gunmetal --save-load 1 --save-load-ticks 60)
    set_tests_properties(cli_save_load_ticks PROPERTIES PASS_REGULAR_EXPRESSION
        "save-load: after ticks pos=\\(1156\\.355, 797\\.276, 48\\.000\\) armor=10000")
    gm_label(cli_save_load_ticks quick)
    add_test(NAME cli_save_load_bad COMMAND gunmetal --save-load 99)
    set_tests_properties(cli_save_load_bad PROPERTIES PASS_REGULAR_EXPRESSION
        "\\[error\\] save-load: save slot 99 out of range \\[0, 19\\]")
    gm_label(cli_save_load_bad quick)
endif()

# M6.1.3: shield/armor regeneration in the tick (docs/REGEN.md).  The shipped
# opcode 174 writes two doubles that 1.36 never reads, so the rule is a model:
# the test pins the `(19, 3000.0, 50.0)` data fact, the pure delay/rate/carry/
# cap arithmetic and the negatives, then on real dm1 + level0 ROCKET proves the
# delay window after a hit, the per-tick shield growth, the restart after a
# second hit, `damage_taken()` monotonicity, the default-off parity with
# `test_sim_damage` (120 idle ticks keep the gauges frozen) and the replay.
add_executable(gm_regen_tests test_regen.cpp)
target_link_libraries(gm_regen_tests PRIVATE gm_engine)
add_test(NAME regen COMMAND gm_regen_tests)
gm_label(regen quick)

# M9.2 slice 2: the deathmatch rules of docs/DM_RULES.md as pure match state
# (game/dm_state.h).  No game files: modes/mp_flags, the `player_killed`
# frag rules of 0x4536e0 (killer +1, team-kill -1, suicide/world -1 only from
# a positive score), the 136-tick respawn (§4.3), frag/time limits (model, the
# shipped DM has none), the handicap ladder of §5 and the negatives.
add_executable(gm_dm_state_tests test_dm_state.cpp)
target_link_libraries(gm_dm_state_tests PRIVATE gm_engine)
add_test(NAME dm_state COMMAND gm_dm_state_tests)
gm_label(dm_state quick)

# M9.1 slice 2: campaign progress (docs/CAMPAIGN_STATE.md) as pure state
# (game/campaign_state.h).  The counter ds:0x80f9cc (new game 1, save clamp
# >= 1, the 0x457b26 increment), the 36-row table of the 66 NEW_LEVEL /
# NEXT_LEVEL sites (60 + 6: five HQs + level19), a full 24-mission walk with
# 23 HQ visits, the 0x44295e restart, objectives 25/51/35/48, secrets 101/105
# and a byte-exact GUNMETAL.HI round-trip (576 bytes, 9 records) plus negatives.
add_executable(gm_campaign_state_tests test_campaign_state.cpp)
target_link_libraries(gm_campaign_state_tests PRIVATE gm_engine)
add_test(NAME campaign_state COMMAND gm_campaign_state_tests)
set_tests_properties(campaign_state PROPERTIES PASS_REGULAR_EXPRESSION
    "campaign_state: ok \\([0-9]+ checks\\) counter=1\\.\\.25 missions=24 next_level_sites=6 transitions=66 hq_visits=23 hi_records=[09]")
gm_label(campaign_state quick)

# M8.4/M9.1 slice 2: the level-selection screen (ui/level_select.h,
# docs/LEVEL_SELECT.md).  36 rows (24 missions, 5 HQs, 3 secret, 4 DM) in
# campaign order, 101 declared secrets, unlock thresholds from the mission
# counter (1/8/17/25 -> 7/15/27/36 selectable, 0/7/16/24 completed),
# best times of the real GUNMETAL.HI (TESTING RANGE 18, KELLER CORE 1570,
# -1 without a record), clamped cursor, launch/locked/back events, negatives.
add_executable(gm_level_select_tests test_level_select.cpp)
target_link_libraries(gm_level_select_tests PRIVATE gm_engine)
add_test(NAME level_select COMMAND gm_level_select_tests)
set_tests_properties(level_select PROPERTIES PASS_REGULAR_EXPRESSION
    "level_select: ok \\([0-9]+ checks\\) levels=36 missions=24 hq=5 secret=3 dm=4 declared=101 first=level1 last=dm4 hi=[01]")
gm_label(level_select quick)

# M6.4.5 slice 2: the objective mask and the secret tally as pure state
# (game/objectives.h, docs/OBJECTIVES_STATE.md).  `DEFINE_LEVEL_OBJECTIVES`
# (103) declare(0..30) and the `0x45bdf6` cap, `OR_LEVEL_OBJECTIVES_BITS`
# (104) idempotent bits with no bits outside the declaration, condition 7
# (`0x45e9f0`: 0 declared false, all declared bits true, high bits ignored),
# the block 11 / entity -3 / opcode 0x68 mask round-trip including foreign bits,
# `0x4536b0` tally 101/101 with the level3 8/6 and level15 4/5 data anomalies,
# the two resets (`0x45280f` tally, `0x45708a` declared), and the text
# negatives.  No game files are read.
add_executable(gm_objectives_tests test_objectives.cpp)
target_link_libraries(gm_objectives_tests PRIVATE gm_engine)
add_test(NAME objectives COMMAND gm_objectives_tests)
set_tests_properties(objectives PROPERTIES PASS_REGULAR_EXPRESSION
    "objectives: ok \\([0-9]+ checks\\) declare=0\\.\\.30")
gm_label(objectives quick)

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
    # M11.2: lightmaps (.sbc) - fill/scale/import with an oracle check.
    add_test(NAME lightmapedit_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/lightmapedit.py
                     --self-test)
    set_tests_properties(lightmapedit_selftest PROPERTIES PASS_REGULAR_EXPRESSION
        "70/70 passed")
    gm_label(lightmapedit_selftest quick)
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
    # M11.4: trace/breakpoints/watch/CFG over the reference interpreter
    # (docs/SCRIPT_DEBUG.md); the self-test is synthetic, no game needed.
    # M3.2.1: C++ structs, the Python parsers and the shipped data must agree
    # with the declared schema (docs/SCHEMA.md).
    # M3.2.7: minimal synthetic fixtures of every format; the engine reads the
    # same bytes (`fixtures_check` consumes the dir `fixtures_write` produced).
    add_test(NAME fixtures_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/fixtures.py
                     --self-test)
    set_tests_properties(fixtures_selftest PROPERTIES PASS_REGULAR_EXPRESSION
        "status: ok")
    gm_label(fixtures_selftest quick)
    add_test(NAME fixtures_write
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/fixtures.py
                     --write ${CMAKE_BINARY_DIR}/fixtures)
    set_tests_properties(fixtures_write PROPERTIES FIXTURES_SETUP gm_fixtures)
    gm_label(fixtures_write quick)
    add_test(NAME fixtures_check
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/fixtures.py
                     --check ${CMAKE_BINARY_DIR}/fixtures --engine
                     --binary $<TARGET_FILE:gunmetal>)
    set_tests_properties(fixtures_check PROPERTIES FIXTURES_REQUIRED gm_fixtures
        PASS_REGULAR_EXPRESSION "status: ok")
    gm_label(fixtures_check quick)
    add_test(NAME schema_check_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/schema_check.py
                     --self-test)
    set_tests_properties(schema_check_selftest PROPERTIES PASS_REGULAR_EXPRESSION
        "19/19 checks passed")
    gm_label(schema_check_selftest quick)
    add_test(NAME schema_check
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/qa/schema_check.py
                     --check --strict)
    set_tests_properties(schema_check PROPERTIES PASS_REGULAR_EXPRESSION
        "расходится 0")
    gm_label(schema_check quick)
    add_test(NAME sbtdbg_selftest
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtdbg.py
                     --self-test)
    set_tests_properties(sbtdbg_selftest PROPERTIES PASS_REGULAR_EXPRESSION
        "26/26 self-tests passed")
    gm_label(sbtdbg_selftest quick)
    # A real level: the init script runs to the end, and a breakpoint stops
    # exactly on the requested instruction.
    add_test(NAME sbtdbg_run
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtdbg.py
                     --run level0 --script main --trace-limit 5)
    set_tests_properties(sbtdbg_run PROPERTIES PASS_REGULAR_EXPRESSION
        "stop reasons: end=1")
    gm_label(sbtdbg_run quick)
    add_test(NAME sbtdbg_break
             COMMAND ${GM_PYTHON3} ${CMAKE_SOURCE_DIR}/tools/editor/sbtdbg.py
                     --run level0 --script off:0xbd8b --source-player 0
                     --break-offset 0xbdb2 --continue 2)
    set_tests_properties(sbtdbg_break PROPERTIES PASS_REGULAR_EXPRESSION
        "player.cash 0 -> 20000")
    gm_label(sbtdbg_break quick)
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
    foreach(tool campaign_flow chassis dm_rules economy effects equipment
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
set(GM_CHECK_DEPENDS gm_tests gm_sou2_tests gm_sou1_tests gm_sof_tests gm_hud_tests gm_sbv_video_tests gm_sbtvm_tests gm_vm_tests gm_fuzz_tests gm_save_tests gm_doors_tests gm_frontend_menu_tests gm_text_layout_tests gm_sml_pager_tests gm_pager_render_tests gm_briefing_tests gm_briefing_render_tests gunmetal)
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

# M6.2.4 slice 2: runtime equipment state (docs/EQUIPMENT_STATE.md).  Real
# level0/dm1: 13/12 records, 12 usable, the full event map of §3, activation
# (success, empty pool + E6, model cooldown, flags & 0x04, already on), tick()
# over cooldowns only, consume() over the shared ammo pool (§2.1: the active
# bit stays), and the refusals with texts.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx")
    add_executable(gm_equipment_state_tests test_equipment_state.cpp)
    target_link_libraries(gm_equipment_state_tests PRIVATE gm_engine)
    add_test(NAME equipment_state COMMAND gm_equipment_state_tests)
    gm_label(equipment_state quick)
endif()

# M6.2.4 slice 2: activation (op 130) and the per-tick ammo drain of
# `FUN_00445fa0` as a pure machine (docs/EQUIPMENT_ACT.md).  Real
# level0/level20/dm1: 13/13/12 records, `BALLAST` only in the two 13-record
# files, the whole 36-file corpus (434 records, 13 names), `1 << id` masks,
# `ammo[E8] -= E7*ticks`, one E6 per drained activation, two ammo types, the
# negatives with texts and determinism.
if(EXISTS "/mnt/d/games/Gunmetal 3dfx")
    add_executable(gm_equipment_act_tests test_equipment_act.cpp)
    target_link_libraries(gm_equipment_act_tests PRIVATE gm_engine)
    add_test(NAME equipment_act COMMAND gm_equipment_act_tests)
    gm_label(equipment_act quick)
endif()

# M9.5 slice 1: the death/respawn/"game over" flow as pure state
# (game/game_flow.h, docs/GAME_FLOW.md).  No game files: `action_state = 7`
# for 136 ticks (0x443b0a), the one-tick respawn due (0x452670), the modelled
# lives/restart prompt, prompt_restart (0x443af9), return_to_hq (0x4428f0(1)),
# the negatives/clamps, zero-length timers and determinism.
add_executable(gm_game_flow_tests test_game_flow.cpp)
target_link_libraries(gm_game_flow_tests PRIVATE gm_engine)
add_test(NAME game_flow COMMAND gm_game_flow_tests)
gm_label(game_flow quick)

# M6.2.6 slice 2: money/shop state as a pure machine (docs/ECONOMY_STATE.md).
# Synthetic: exact `buy`/`sell`/`sell_all` arithmetic, the failed-funds no-op
# and counter (§4.2/§4.3), mass = Σ mass*count and the opt-in model limit
# (§6: the original does not check it), negatives with texts, determinism.
# Real level0/dm1: the shipped prices/masses of §3 (7 equipment records, 4
# weapons, 3 ammo lots), `DEFINE_SHIELDS (50, 0)`, the four top-level chassis
# and the §12 sums (1 100 000, 2185/1335).  No installation -> file half is
# skipped with a printed reason.
add_executable(gm_economy_state_tests test_economy_state.cpp)
target_link_libraries(gm_economy_state_tests PRIVATE gm_engine)
add_test(NAME economy_state COMMAND gm_economy_state_tests)
set_tests_properties(economy_state PROPERTIES PASS_REGULAR_EXPRESSION
    "economy_state: [0-9]+ checks, ok")
gm_label(economy_state quick)

# M6.2.6 slice 3: pickup rules as a pure machine (docs/PICKUP_STATE.md).
# Synthetic: armor heal/clamp at armor_max 10000 (op 112), the four-plate
# shield rotation 0→1→2→3 up to shield_max 8000 (op 113, 0x45b177), idempotent
# weapon/equipment/key ORs (108/111/106), ammo stacking and clamp (109), cash
# (114), the uncapped secret tally (95), and the opt-in mass model (§6).
# Real level0/dm1: item-op counts (41/170), the trigger/radius split, the
# shields/marker/virus definitions and the four chassis caps.  No installation
# -> the file half is skipped with a printed reason.
add_executable(gm_pickup_tests test_pickup.cpp)
target_link_libraries(gm_pickup_tests PRIVATE gm_engine)
add_test(NAME pickup COMMAND gm_pickup_tests)
set_tests_properties(pickup PROPERTIES PASS_REGULAR_EXPRESSION
    "pickup: [0-9]+ checks, ok")
gm_label(pickup quick)

# M4.1.6 slice 2: depth fog of the software frame path (docs/FOG.md,
# docs/FOG_CPP.md).  `fog_level` = clamp(63-trunc(z),4,63) on the §3 anchors,
# `fog_z` = |(64/a0)*(e2.(p-eye))|, the 256x256 `lighttable` (`0x4201fe`) built
# from the real FRONTEND/GAMEPAL with its FNV-1a64, first white index (96), the
# 66 all-white rows 62..127 and every entry against an independent formula,
# then `fog_frame` on 256- and 320x200-byte frames with Python FNV goldens.
# Text negatives: a0 <= 0 / NaN, non-finite coordinates, level outside 0..255,
# palette byte > 63, null palette.  Skips the palette half, with a printed
# reason, when the game is not installed.
add_executable(gm_fog_tests test_fog.cpp)
target_link_libraries(gm_fog_tests PRIVATE gm_engine)
add_test(NAME fog COMMAND gm_fog_tests)
gm_label(fog quick)

# M4.4.5 slice 2: cockpit icon assets and the compass model
# (docs/HUD_ICONS_CPP.md).  Synthetic half needs no game files: an empty
# `Arch`, a hand-built BIGFILE pair holding a 2x3 BMP, the refusal texts for a
# bad id / `alt=true` / an empty archive / a missing asset / a non-BMP blob,
# and the compass model (bearing 0/90/180/270, normalization, non-finite
# input).  Shipped half reads FRONTEND/BMP.IDX (143 entries) and level0.sbt:
# the 19-entry weapon table is compared with the real DEFINE_WEAPONS block,
# every id is re-read and re-parsed through BigFile+parse_bmp, the 12 HUD
# equipment slots and equip12 (BALLAST) are resolved, and determinism is
# checked.  Skips the shipped half cleanly when the game is not installed.
add_executable(gm_hud_icons_tests test_hud_icons.cpp)
target_link_libraries(gm_hud_icons_tests PRIVATE gm_engine)
add_test(NAME hud_icons COMMAND gm_hud_icons_tests)
gm_label(hud_icons quick)

# M4.4.5 slice 3: the composed cockpit frame (docs/COCKPIT.md).  Shipped half
# reads FRONTEND/BMP.IDX and compares the panel band with an independent
# PANEL blit, pins the element/ink/panel numbers, the armour scale boundaries,
# the icon/number switches and determinism.  Synthetic half (no game files)
# covers the missing index/PANEL, a broken palette, a wrong-sized PANEL and the
# armour clamp.
add_executable(gm_cockpit_tests test_cockpit.cpp)
target_link_libraries(gm_cockpit_tests PRIVATE gm_engine)
add_test(NAME cockpit COMMAND gm_cockpit_tests)
gm_label(cockpit quick)

# M2.2/M11.5 slice 2: `.sub` substitution tables (`res/sub.*`, docs/SUB_CPP.md).
# Both layouts (counted `<weight> <from> <to>` and the tab-separated `sounds`
# pairs), the first-row-wins rule of 0x42307a and the `ram_class <= weight`
# filter of 0x42302c; the shipped files (119/64/161 pairs) and `textures.sub`
# bound to the 381 texture references of level0.sbw (16 rows, 15 slots, 378 ->
# 373 unique names); the refusals (empty/non-.sub/count/weight/name/format)
# and the warnings (counter mismatch, `from == to`).  Skips the file half
# cleanly when the game is not installed.
add_executable(gm_sub_tests test_sub.cpp)
target_link_libraries(gm_sub_tests PRIVATE gm_engine)
add_test(NAME sub COMMAND gm_sub_tests)
gm_label(sub quick)

# M12.5/M1.5 slice 1: the `GUNMETAL.CFG` options image (game/game_cfg.h,
# docs/GAME_CFG.md, docs/CONFIG.md §2/§3).  The shipped 8192-byte file read
# only: magic + 0x1A + NUL, the sixteen proven fields (16/20/800x600x8/3/1/…),
# a byte-exact 8192-byte round-trip, `set_field` changing only its own four
# bytes and not `play_flags_p2`, the proven boundaries, read-only and unproven
# names, the size/header/mask refusals, the "never write into the installation"
# guard and `CONTROLS.CFG` (av1.91) being refused as opaque.  Without the
# installation the same checks run on a synthetic image.
add_executable(gm_game_cfg_tests test_game_cfg.cpp)
target_link_libraries(gm_game_cfg_tests PRIVATE gm_engine)
add_test(NAME game_cfg COMMAND gm_game_cfg_tests)
set_tests_properties(game_cfg PROPERTIES PASS_REGULAR_EXPRESSION
    "game_cfg: [0-9]+ checks, ok")
gm_label(game_cfg quick)

# M8.6 slice 2: reusable HUD overlay (`render/hud_overlay.*`, docs/HUD_OVERLAY.md).
# One `.sof` film frame or a stack of them composed onto a finished RGB frame:
# scale/opacity/transparent-index options, edge clipping, and the stats
# invariant drawn + skipped + clipped == layer area.  The shipped AMMO4 film is
# compared byte for byte with `hud_player::draw` (what `main.cpp::overlay_hud`
# calls) at (14,125), plus two pinned FNV-1a64 numbers.
add_executable(gm_hud_overlay_tests test_hud_overlay.cpp)
target_link_libraries(gm_hud_overlay_tests PRIVATE gm_engine)
add_test(NAME hud_overlay COMMAND gm_hud_overlay_tests)
gm_label(hud_overlay quick)

# M8.8 slice 10: one frame of screen 26, `JOYSTICK CENTERING`
# (docs/JOYSTICK_CENTERING_RENDER.md, docs/UI_SCREENS.md §2/§3.8).  A purely static
# screen: title font 5 centred at y 14 and five information lines at
# (36,25)/(36,35)/(36,45)/(41,86)/(41,97), because the handler writes rows 1
# and item step 0 while all five lines pass item 0.  There are no `%s`
# substitutions; the frame-changing quirk is that cursor 0 whitens the whole
# body (font 4).  No MENUARRW, PLATE as an independently decoded background,
# the exact fade levels and the refusals.  Every band is compared with an
# independent blit; bboxes and diff counts are printed.  Skips the pixel runs
# cleanly when the game is not installed.
add_executable(gm_joystick_centering_render_tests test_joystick_centering_render.cpp)
target_link_libraries(gm_joystick_centering_render_tests PRIVATE gm_engine)
add_test(NAME joystick_centering_render COMMAND gm_joystick_centering_render_tests)
set_tests_properties(joystick_centering_render PROPERTIES PASS_REGULAR_EXPRESSION
    "joystick_centering_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(joystick_centering_render quick)

# M8.8 slice 20: one frame of screen 32, `PLAY OPTIONS`
# (docs/PLAY_OPTIONS_RENDER.md, docs/UI_SCREENS.md §2/§3.4).  rows 8, step 15,
# title font 5 centred at y 11, the labels at x 25 (items 0..5: y 21/32/43/60/
# 78/95) and x 13 (camera y 105, `Done` y 114), the second lines `Ammo`/
# `Ammo Depleted`/`Looking` at y 51/68/86, six `CHECK`/`NOCHECK` boxes at x 16
# (anchors y 13/24/35/52/70/87, unchecked 8 rows lower), the six bits of
# `play_flags_p1`, the cursor item in font 4 (the in-game camera line uses font
# 8 and is never whitened), no MENUARRW/sliders, PLATE as an independently
# decoded background, the exact fade levels and the refusals.  Every band is
# compared with an independent blit; bboxes, per-box inks and diff counts are
# printed.  Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_play_options_render_tests test_play_options_render.cpp)
target_link_libraries(gm_play_options_render_tests PRIVATE gm_engine)
add_test(NAME play_options_render COMMAND gm_play_options_render_tests)
set_tests_properties(play_options_render PROPERTIES PASS_REGULAR_EXPRESSION
    "play_options_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(play_options_render quick)

# M8.8 slice 23: one frame of screen 31, `NAME OF NEW KEY SET`
# (docs/NEW_KEYSET_RENDER.md, docs/UI_SCREENS.md §2/§3.7).  The map lists no
# handler; the whole frame is painted by 0x48ebb0 (argument: the key-set
# index), called from 0x48d1a5 (entering the state) and 0x48f432 (the state-31
# branch of the input pass 0x48efc0).  rows 1 / step 0, PLATE 196x131 copy
# 0x486050, title `NAME OF NEW KEY SET` (0x4bdd8c) font 5 centred at y 14, the
# name (0x6c2058 + set*0x108) in font 3 at x = 98 - width/2, y 28, the hollow
# caret frame 8x17 on row 27 (0x485d40, width 16/6 over M/W/I), the four font-0
# prompts at x 26 (y 48/58/87/102), the 15-character limit of 0x405b20 and the
# `NEW SET` fallback (0x4bdd3c).  Every pixel of the 320x200 canvas is compared
# with an independent composition (black or the decoded PLATE), a 30-character
# name is cut to 15, `caret = false` removes exactly 46 px, PLATE is decoded
# independently, the exact fade levels and the refusals are pinned; bboxes,
# inks and diff counts are printed.  Skips the pixel runs cleanly when the game
# is not installed.
add_executable(gm_new_keyset_render_tests test_new_keyset_render.cpp)
target_link_libraries(gm_new_keyset_render_tests PRIVATE gm_engine)
add_test(NAME new_keyset_render COMMAND gm_new_keyset_render_tests)
set_tests_properties(new_keyset_render PROPERTIES PASS_REGULAR_EXPRESSION
    "new_keyset_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(new_keyset_render quick)

# M8.8 slice 18: one frame of screen 11, `SOUND OPTIONS`
# (docs/SOUND_OPTIONS_RENDER.md, docs/UI_SCREENS.md §2/§3.2).  rows 7, step 10,
# title font 5 centred at y 14, `Music Volume:` (18,27) with `SLIDE` (68,37) +
# `KNOB` (69 + 27*volume/10), `Effects Volume:` (18,47) with `SLIDE` (68,57),
# `Switch Left Right` (30,67) with the `CHECK`/`NOCHECK` box at (20,59),
# `Done` (18,84), the cursor item in font 4, the `music_low_memory` and
# `sound_enabled = false` layouts, no MENUARRW, PLATE as an independently
# decoded background, the exact fade levels and the refusals.  Every band and
# widget rectangle is compared with an independent blit/composite; knob shifts,
# checkbox toggles, bboxes and diff counts are printed.  Skips the pixel runs
# cleanly when the game is not installed.
add_executable(gm_sound_options_render_tests test_sound_options_render.cpp)
target_link_libraries(gm_sound_options_render_tests PRIVATE gm_engine)
add_test(NAME sound_options_render COMMAND gm_sound_options_render_tests)
set_tests_properties(sound_options_render PROPERTIES PASS_REGULAR_EXPRESSION
    "sound_options_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(sound_options_render quick)

# M8.8 slice 19: one frame of screen 12, `GRAPHICS OPTIONS`
# (docs/GRAPHICS_OPTIONS_RENDER.md, docs/UI_SCREENS.md §2/§3.3).  rows 6, step
# 15, title font 5 centred at y 11, the 0x486500 mode line (38,24), `Border:`
# (38,39) with `SLIDE` (88,39) + `KNOB` (89 + 6*border), the gamma line (38,54),
# `Reflections` (50,69) with the `CHECK`/`NOCHECK` box at (40,61), the light-map
# label (38,84) and `Done` (38,98), the cursor item in font 4, no MENUARRW,
# PLATE as an independently decoded background, the exact fade levels and the
# refusals.  Every band and widget rectangle is compared with an independent
# blit/composite; knob shifts, checkbox toggles, bboxes and diff counts are
# printed.  Skips the pixel runs cleanly when the game is not installed.
add_executable(gm_graphics_options_render_tests test_graphics_options_render.cpp)
target_link_libraries(gm_graphics_options_render_tests PRIVATE gm_engine)
add_test(NAME graphics_options_render COMMAND gm_graphics_options_render_tests)
set_tests_properties(graphics_options_render PROPERTIES PASS_REGULAR_EXPRESSION
    "graphics_options_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(graphics_options_render quick)

# M8.8 slice 22: one frame of screens 24/25, the local player name entry
# (docs/PLAYER_NAME_RENDER.md, docs/UI_SCREENS.md §2).  Handler
# 0x489020(player): rows 1 / step 0 (0x489023/0x48902d), the PLATE copy
# 0x486050, a centred title in font 5 at y 14 (`LOCAL PLAYER ONE NAME`
# 0x4bd730 for 0, `LOCAL PLAYER TWO NAME` 0x4bd748 otherwise), the 16x16
# `FONTS2` name line centred on x 98 at y 28, the hollow 17 px caret box of
# 0x485d40 at plate row 27 (width 16/6/8 from the character at `[0x683f90]`)
# and six hint lines in font 0 at x 16, y 48/56/67/75/89/102.  The name buffer
# is 17 bytes (0x682f60 + player*0x11) but the editor 0x405b20 is capped at 11
# bytes including NUL (push 0xb at 0x48f4cd), so 10 characters are reachable.
# The test pins both titles, the name band, the 10-character cut, the caret
# on/off and width rule, PLATE as an independently decoded background, the
# exact fade levels and the refusals.  Skips the pixel runs cleanly when the
# game is not installed.
add_executable(gm_player_name_render_tests test_player_name_render.cpp)
target_link_libraries(gm_player_name_render_tests PRIVATE gm_engine)
add_test(NAME player_name_render COMMAND gm_player_name_render_tests)
set_tests_properties(player_name_render PROPERTIES PASS_REGULAR_EXPRESSION
    "player_name_render: (ok \\([0-9]+ checks\\)|game not installed)")
gm_label(player_name_render quick)
