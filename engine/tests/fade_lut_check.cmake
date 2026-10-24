# Checks the exact HUD fade tables through the CLI (`--fade-lut`, docs/HUD_LUT.md
# §2c, docs/PAGER_RENDER.md §2.6):
#   * level 11 is the identity -> the frame is byte-identical to no fade;
#   * level 13 is *not* the identity -> the frame differs (the model clamps
#     12..15 to 11, so this is exactly what the exact tables buy us).
#
# Every front-end frame that takes `--fade-lut` is checked.  `LUT_ENV` (optional)
# pins `GM_HUD_LUT`; pointing it at a missing path makes the engine build the
# tables from FRONTEND/GAMEPAL instead.
#
# Invoked as `cmake -DGUNMETAL=... -DGAME=... -DOUT=... [-DLUT_ENV=...]
# -P fade_lut_check.cmake`.

if(NOT DEFINED GUNMETAL OR NOT DEFINED GAME OR NOT DEFINED OUT)
    message(FATAL_ERROR "fade_lut_check: GUNMETAL, GAME and OUT are required")
endif()

file(MAKE_DIRECTORY "${OUT}")

set(ENV_PREFIX "")
if(DEFINED LUT_ENV)
    set(ENV_PREFIX "${CMAKE_COMMAND}" -E env "GM_HUD_LUT=${LUT_ENV}")
endif()

function(run_frame png)
    execute_process(
        COMMAND ${ENV_PREFIX} "${GUNMETAL}" --game "${GAME}" ${ARGN} --png "${png}"
        RESULT_VARIABLE rc
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "fade_lut_check: ${png} failed with ${rc}")
    endif()
endfunction()

# expect(name, base args...)
function(expect name)
    run_frame("${OUT}/${name}-none.png" ${ARGN})
    run_frame("${OUT}/${name}-11.png" ${ARGN} --fade-lut 11)
    run_frame("${OUT}/${name}-13.png" ${ARGN} --fade-lut 13)

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${OUT}/${name}-none.png" "${OUT}/${name}-11.png"
        RESULT_VARIABLE same)
    if(NOT same EQUAL 0)
        message(FATAL_ERROR "fade_lut_check: ${name}: level 11 is not the identity")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files
                "${OUT}/${name}-none.png" "${OUT}/${name}-13.png"
        RESULT_VARIABLE differs)
    if(differs EQUAL 0)
        message(FATAL_ERROR "fade_lut_check: ${name}: level 13 did not change the frame")
    endif()
endfunction()

expect(pager --pager help.sme --background)
expect(briefing --briefing level1 --background)
expect(level-select --level-select)
expect(save-menu --save-menu)
expect(start-game --start-game)
expect(keycfg --keycfg PERFECT)
expect(menu --menu --menu-screen 10)
expect(game-type --game-type)
expect(difficulty --difficulty)
expect(local-players --local-players)
expect(quit --quit)
expect(video-mode --video-mode)
expect(restart --restart)
expect(video-mode-list --video-mode-list)
expect(mouse-setup --mouse-setup)
expect(joystick-centering --joystick-centering)
expect(match-setup --match-setup)
expect(key-jamming --key-jamming)
expect(ingame-menu --ingame-menu)
expect(lay-bucks --lay-bucks)
expect(save-name --save-name)
expect(error --error)
expect(player-setup --player-setup)
expect(control-options --control-options)
expect(sound-options --sound-options)
expect(graphics-options --graphics-options)
expect(play-options --play-options)
expect(player-name --player-name)
expect(new-keyset --new-keyset)

message(STATUS "fade_lut_check: 29 frames: level 11 = identity, level 13 differs; OK")
