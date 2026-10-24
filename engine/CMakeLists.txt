add_library(gm_engine STATIC
    src/camera/camera.cpp
    src/core/bytes.cpp
    src/core/log.cpp
    src/core/path.cpp
    src/core/config.cpp
    src/core/time.cpp
    src/audio/sou1.cpp
    src/audio/mixer.cpp
    src/audio/music.cpp
    src/audio/sample_cache.cpp
    src/audio/sound_policy.cpp
    src/audio/playlist.cpp
    src/platform/window.cpp
    src/platform/audio.cpp
    src/res/anim.cpp
    src/anim/soo_player.cpp
    src/res/level.cpp
    src/res/sbv.cpp
    src/res/sbv_video.cpp
    src/res/cache.cpp
    src/render/render.cpp
    src/render/camera_model.cpp
    src/render/scale.cpp
    src/render/postfx.cpp
    src/render/fog.cpp
    src/render/collide.cpp
    src/render/collide_grid.cpp
    src/render/collide_overlap.cpp
    src/player/controller.cpp
    src/input/controls.cpp
    src/render/text_layout.cpp
    src/render/hud.cpp
    src/render/sprite.cpp
    src/render/hud_player.cpp
    src/render/hud_bind.cpp
    src/render/hud_compose.cpp
    src/render/hud_icons.cpp
    src/render/hud_overlay.cpp
    src/render/cockpit.cpp
    src/render/upscale.cpp
    src/render/hud_lut.cpp
    src/render/fx_frame.cpp
    src/render/texture_anim.cpp
    src/ui/screen.cpp
    src/ui/sml_pager.cpp
    src/ui/briefing.cpp
    src/ui/briefing_render.cpp
    src/ui/pager_render.cpp
    src/ui/menu_render.cpp
    src/ui/frontend_menu.cpp
    src/ui/ui_sounds.cpp
    src/ui/save_slots.cpp
    src/ui/save_menu.cpp
    src/ui/save_menu_render.cpp
    src/ui/start_game_render.cpp
    src/ui/game_type_render.cpp
    src/ui/keycfg_render.cpp
    src/ui/records.cpp
    src/ui/killed.cpp
    src/ui/level_select.cpp
    src/ui/level_select_render.cpp
    src/ui/difficulty_render.cpp
    src/ui/local_players_render.cpp
    src/ui/quit_render.cpp
    src/ui/video_mode_render.cpp
    src/ui/video_mode_list_render.cpp
    src/ui/restart_render.cpp
    src/ui/mouse_setup_render.cpp
    src/ui/joystick_centering_render.cpp
    src/ui/match_setup_render.cpp
    src/ui/key_jamming_render.cpp
    src/ui/lay_bucks_render.cpp
    src/ui/error_render.cpp
    src/ui/player_setup_render.cpp
    src/ui/control_options_render.cpp
    src/ui/save_name_render.cpp
    src/ui/player_name_render.cpp
    src/ui/ingame_menu_render.cpp
    src/ui/sound_options_render.cpp
    src/ui/graphics_options_render.cpp
    src/ui/play_options_render.cpp
    src/ui/new_keyset_render.cpp
    src/res/bigfile.cpp
    src/res/bmp.cpp
    src/res/png.cpp
    src/res/sbm.cpp
    src/res/sbw.cpp
    src/res/sub.cpp
    src/res/sbc.cpp
    src/res/sob.cpp
    src/res/sbt.cpp
    src/audio/engine_3d.cpp
    src/res/sound_catalog.cpp
    src/res/sbtvm.cpp
    src/res/vm_exec.cpp
    src/res/save.cpp
    src/res/save_write.cpp
    src/game/state.cpp
    src/game/events.cpp
    src/game/weapons.cpp
    src/game/line_of_sight.cpp
    src/game/trigger.cpp
    src/game/projectile.cpp
    src/game/runtime.cpp
    src/game/regen.cpp
    src/game/sim_player.cpp
    src/game/save_load.cpp
    src/game/save_action.cpp
    src/game/equipment.cpp
    src/game/equipment_state.cpp
    src/game/equipment_act.cpp
    src/game/economy_state.cpp
    src/game/pickup.cpp
    src/game/player_types.cpp
    src/game/chassis_loadout.cpp
    src/game/chassis_physics.cpp
    src/game/effect_anim.cpp
    src/game/dm_state.cpp
    src/game/game_flow.cpp
    src/game/campaign_state.cpp
    src/game/objectives.cpp
    src/game/game_cfg.cpp
    src/res/scene.cpp
    src/res/sou.cpp
    src/res/sou1.cpp
    src/res/sou2.cpp
    src/res/sof.cpp
    src/res/soe.cpp
    src/res/text.cpp
    src/res/sml.cpp
    src/world/doors.cpp
    src/world/moving_collider.cpp
    src/world/things.cpp
    src/world/ai.cpp
    src/world/ai_move.cpp
    src/world/thing_motion.cpp
    src/world/thing_anim.cpp
)

target_include_directories(gm_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(gm_engine PUBLIC cxx_std_20)

# Optional build accelerators (GM_USE_PCH/GM_UNITY_BUILD, both off by default).
# A clean build already finishes in ~3 s here, so these exist for slower hosts;
# see CMakeLists.txt for the measurements.  PCH is a measured win per TU (-35%)
# and is enabled by the dev/test presets.  Unity builds are EXPERIMENTAL and do
# not compile as-is: several .cpp files define identically named file-local
# helpers (two `Reader`s in the sbtvm batch), which would need renames in
# engine/src.  The option is kept so that work can be measured later.
if(GM_UNITY_BUILD)
    set_target_properties(gm_engine PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_MODE BATCH
        UNITY_BUILD_BATCH_SIZE 8)
endif()

if(GM_USE_PCH)
    target_precompile_headers(gm_engine PRIVATE
        <algorithm>
        <cstddef>
        <cstdint>
        <cstdio>
        <cstring>
        <optional>
        <span>
        <string>
        <string_view>
        <vector>
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/bytes.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/log.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/path.h)
endif()

if(GM_WARNINGS_AS_ERRORS)
    set(GM_WARN_FLAGS -Werror)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(gm_engine PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off
                                            ${GM_WARN_FLAGS})
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
