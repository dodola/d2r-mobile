cmake_minimum_required(VERSION 3.22)
project(d2r_mobile_android LANGUAGES C ASM)
add_link_options(-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a" OR ANDROID_PLATFORM_LEVEL LESS 29)
  message(FATAL_ERROR "The native guest host requires Android 29+ arm64-v8a")
endif()

add_library(dm_android_gfx OBJECT
  ../gfx/nvn/hle_nvn.c ../gfx/nvn/nvn_entry.S ../gfx/nvn/nvn_thunks.S
  ../gfx/nvn/nvn_tex.c ../gfx/nvn/nvn_dirty.c ../gfx/nvn/nvn_vk.c
  ../gfx/tex/nvn_deswizzle.c ../gfx/tex/nvn_bcdec.c nvn_platform_clock.c nvn_translation_queue.c
  ../gfx/hle_vi.c ../gfx/hle_nv.c movie_start.c)
target_include_directories(dm_android_gfx PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_gfx PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffixed-x18)
target_compile_definitions(dm_android_gfx PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_EXPECT_VULKAN_BACKEND=1)
set_target_properties(dm_android_gfx PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dm_android_gfx PUBLIC vulkan android log dl z m)
set(DM_HOST ../host)

add_library(dm_android_core OBJECT
  ${DM_HOST}/loader.c ${DM_HOST}/hle.c ${DM_HOST}/entry.S
  ${DM_HOST}/thread.c ${DM_HOST}/hle_os.c ${DM_HOST}/hle_thread.c
  ${DM_HOST}/hle_svc.c ${DM_HOST}/hle_time.c ${DM_HOST}/hle_guestthread.c
  ${DM_HOST}/hle_sdkinit.c ${DM_HOST}/hle_fs.c thread_slots.c services_sdk.c
  ${DM_HOST}/hle_audio.c codec_opus.c
  ${DM_HOST}/hle_hid.c ${DM_HOST}/hle_touch.c input_backend.c
  ../ios/Sources/dm_touch_controls.c touch_controls_android.c
  ${DM_HOST}/hle_oe.c ${DM_HOST}/hle_account.c ${DM_HOST}/hle_net.c
  # Cross-platform host modules shared with the iOS shell.  dm_custom keeps
  # the settings/Apply policy on Android while widget access requires a
  # verified title transport; dm_i18n uses the Android locale fallback and
  # dm_namegen is pure utility code.  Modules that inspect title-private
  # object layouts (vault/pouch/actions/cursor/world, etc.) remain opt-in
  # title adapters below and must not be inferred from another title.
  ${DM_HOST}/dm_hud.c ${DM_HOST}/dm_textbox_core.c ${DM_HOST}/dm_rwlv_core.c
  ${DM_HOST}/dm_custom.c ${DM_HOST}/dm_i18n.c
  ${DM_HOST}/dm_namegen.c
  lifecycle_android.c
  save_notice_android.c
  ${DM_HOST}/hle_lifecycle.c ${DM_HOST}/hle_friends.c ${DM_HOST}/hle_misc.c
  ${DM_HOST}/hle_swkbd.c keyboard_android.c
  services_fs.c services_fs_probe.c services_platform.c diagnostics.c)
target_include_directories(dm_android_core PUBLIC ${DM_HOST} .)
target_compile_features(dm_android_core PUBLIC c_std_11)
# Upstream's Darwin uint64 format specifiers have the same 64-bit ABI on Bionic.
# Keep that source difference separate from actual width/ABI validation.
target_compile_options(dm_android_core PRIVATE -Wall -Wextra -Wno-format -Wno-unused-function -Werror -ffixed-x18)
target_compile_definitions(dm_android_core PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_ANDROID_HOST=1)
set_target_properties(dm_android_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(dm_android_core PUBLIC android log dl m aaudio)
target_link_options(dm_android_core INTERFACE -Wl,-z,nostart-stop-gc)
add_executable(dm_android_host_probe host_probe.c gate_check.S)


target_compile_options(dm_android_host_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18)
target_link_libraries(dm_android_host_probe PRIVATE dm_android_core)


target_link_options(dm_android_host_probe PRIVATE -Wl,-z,max-page-size=16384)
# An explicit standalone test target; never adds test hooks to the game APK.
get_target_property(DM_NVNGFX_TEST_SOURCES dm_android_gfx SOURCES)
add_executable(dm_android_zcull_probe EXCLUDE_FROM_ALL tests/t_nvn_zcull_device.c ${DM_NVNGFX_TEST_SOURCES})
target_include_directories(dm_android_zcull_probe PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_zcull_probe PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffixed-x18)
target_compile_definitions(dm_android_zcull_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_VK_COPY_TEST)
target_link_libraries(dm_android_zcull_probe PRIVATE dm_android_core vulkan android log dl z m)

# Calls only the real NVN texture-view API; no Vulkan initialization or assets.
add_executable(dm_android_view_probe EXCLUDE_FROM_ALL tests/t_nvn_view_device.c)
target_include_directories(dm_android_view_probe PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_view_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_compile_definitions(dm_android_view_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_link_libraries(dm_android_view_probe PRIVATE dm_android_core dm_android_gfx)
add_executable(dm_android_ui_ghost_probe EXCLUDE_FROM_ALL tests/t_ui_ghost_device.c)
target_include_directories(dm_android_ui_ghost_probe PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_ui_ghost_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_compile_definitions(dm_android_ui_ghost_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_link_libraries(dm_android_ui_ghost_probe PRIVATE dm_android_core dm_android_gfx)
get_target_property(DM_MEMORY_TEST_GFX_SOURCES dm_android_gfx SOURCES)
list(FILTER DM_MEMORY_TEST_GFX_SOURCES EXCLUDE REGEX "nvn_vk\\.c$")
add_executable(dm_android_memory_trim_probe EXCLUDE_FROM_ALL tests/t_memory_trim_device.c ${DM_MEMORY_TEST_GFX_SOURCES})
target_include_directories(dm_android_memory_trim_probe PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_memory_trim_probe PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffixed-x18)
target_compile_definitions(dm_android_memory_trim_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_link_libraries(dm_android_memory_trim_probe PRIVATE dm_android_core vulkan android log dl z m)

add_executable(dm_android_draw_probe EXCLUDE_FROM_ALL tests/t_draw_parameters_device.c)
target_compile_options(dm_android_draw_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dm_android_draw_probe PRIVATE vulkan)
add_executable(dm_android_clip_probe EXCLUDE_FROM_ALL tests/t_clip_device.c)
target_compile_options(dm_android_clip_probe PRIVATE -Wall -Wextra -Werror -Wno-misleading-indentation)
target_link_libraries(dm_android_clip_probe PRIVATE vulkan)
add_executable(dm_android_image_view_probe EXCLUDE_FROM_ALL tests/t_image_view_device.c)
target_compile_options(dm_android_image_view_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dm_android_image_view_probe PRIVATE vulkan)

# Shaderless transfer-only presentation format, crop and Y-flip semantics.
add_executable(dm_android_present_probe EXCLUDE_FROM_ALL tests/t_present_device.c)
target_compile_options(dm_android_present_probe PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dm_android_present_probe PRIVATE vulkan m)

# Isolated ARM depth/input attachment ordering test; never linked into the APK.
add_executable(dm_android_depth_input_probe EXCLUDE_FROM_ALL tests/t_depth_input_device.c)

target_compile_options(dm_android_depth_input_probe PRIVATE -Wall -Wextra -Werror)

target_link_libraries(dm_android_depth_input_probe PRIVATE vulkan m)

add_executable(dm_android_input_probe input_probe.c)
target_compile_options(dm_android_input_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18)
target_link_libraries(dm_android_input_probe PRIVATE dm_android_core)
target_link_options(dm_android_input_probe PRIVATE -Wl,-z,max-page-size=16384)
add_executable(dm_android_controller_back_probe EXCLUDE_FROM_ALL tests/t_controller_back.c)
target_compile_options(dm_android_controller_back_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18 -UNDEBUG)
target_link_libraries(dm_android_controller_back_probe PRIVATE dm_android_core)
target_link_options(dm_android_controller_back_probe PRIVATE -Wl,-z,max-page-size=16384)

# Verify legacy touch controls stay usable until a complete private bridge binds.
add_executable(dm_android_touch_bridge_fallback_probe EXCLUDE_FROM_ALL tests/t_touch_bridge_fallback.c)
target_compile_options(dm_android_touch_bridge_fallback_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18)
target_link_libraries(dm_android_touch_bridge_fallback_probe PRIVATE dm_android_core)

# Production focus/async-save lifecycle code with an isolated adapter fixture.
add_executable(dm_android_lifecycle_probe EXCLUDE_FROM_ALL tests/t_lifecycle_device.c)
target_compile_definitions(dm_android_lifecycle_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_lifecycle_probe PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffixed-x18 -ffunction-sections -fdata-sections)
target_link_options(dm_android_lifecycle_probe PRIVATE -Wl,--gc-sections)
target_link_libraries(dm_android_lifecycle_probe PRIVATE dm_android_core)

# Synthetic D2R heap/observer fixture; never linked into either game APK.
add_executable(dm_android_d2r_save_probe EXCLUDE_FROM_ALL tests/t_d2r_save_device.c d2r_save_adapter.c)
target_compile_definitions(dm_android_d2r_save_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_d2r_save_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_save_probe PRIVATE dm_android_core)
add_executable(dm_android_d2r_ui_probe EXCLUDE_FROM_ALL tests/t_d2r_ui_device.c)
target_compile_definitions(dm_android_d2r_ui_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_d2r_ui_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18 -UNDEBUG)
target_link_libraries(dm_android_d2r_ui_probe PRIVATE dl)
add_executable(dm_android_d2r_touch_flow_probe EXCLUDE_FROM_ALL tests/t_d2r_touch_flow_device.c d2r_touch_flow_android.c)
target_compile_definitions(dm_android_d2r_touch_flow_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_d2r_touch_flow_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_touch_flow_probe PRIVATE dm_android_core)
add_executable(dm_android_d2r_touch_shell_probe EXCLUDE_FROM_ALL tests/t_d2r_touch_shell_device.c d2r_touch_shell_android.c)
target_compile_definitions(dm_android_d2r_touch_shell_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_d2r_touch_shell_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_touch_shell_probe PRIVATE dm_android_core)
add_executable(dm_android_d2r_world_overlay_probe EXCLUDE_FROM_ALL tests/t_d2r_world_overlay_device.c d2r_world_overlay_android.c ${DM_HOST}/dm_i18n.c)
target_compile_definitions(dm_android_d2r_world_overlay_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_ANDROID_HOST=1)
target_compile_options(dm_android_d2r_world_overlay_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_world_overlay_probe PRIVATE m)
add_executable(dm_android_d2r_scale3d_probe EXCLUDE_FROM_ALL tests/t_d2r_scale3d_device.c)
target_compile_definitions(dm_android_d2r_scale3d_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_ANDROID_HOST=1)
target_compile_options(dm_android_d2r_scale3d_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-function -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_scale3d_probe PRIVATE m)
add_executable(dm_android_d2r_scale3d_91738_probe EXCLUDE_FROM_ALL tests/t_d2r_scale3d_91738_device.c)
target_compile_definitions(dm_android_d2r_scale3d_91738_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_ANDROID_HOST=1)
target_compile_options(dm_android_d2r_scale3d_91738_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-function -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_scale3d_91738_probe PRIVATE m)
add_executable(dm_android_d2r_hud_probe EXCLUDE_FROM_ALL tests/t_d2r_hud_device.c)
target_compile_definitions(dm_android_d2r_hud_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_d2r_hud_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18 -UNDEBUG)
target_link_libraries(dm_android_d2r_hud_probe PRIVATE dl)
target_link_options(dm_android_d2r_hud_probe PRIVATE -Wl,--export-dynamic-symbol=dm_padmode_rebuilds)
add_executable(dm_android_d2r_custom_probe EXCLUDE_FROM_ALL tests/t_d2r_custom_device.c)
target_compile_definitions(dm_android_d2r_custom_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_d2r_custom_probe PRIVATE -Wall -Wextra -Werror -UNDEBUG -ffixed-x18)
target_link_libraries(dm_android_d2r_custom_probe PRIVATE dl)
add_executable(dm_android_d2r_custom_consumers_probe EXCLUDE_FROM_ALL tests/t_d2r_custom_consumers_device.c)
target_compile_definitions(dm_android_d2r_custom_consumers_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_d2r_custom_consumers_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_custom_consumers_probe PRIVATE dl)

add_executable(dm_android_d2r_autobuff_setting_probe EXCLUDE_FROM_ALL tests/t_d2r_autobuff_setting.c)
target_compile_definitions(dm_android_d2r_autobuff_setting_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_ANDROID_HOST=1)
target_compile_options(dm_android_d2r_autobuff_setting_probe PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffunction-sections -fdata-sections -ffixed-x18)
target_link_options(dm_android_d2r_autobuff_setting_probe PRIVATE -Wl,--gc-sections)
add_executable(dm_android_granny_probe EXCLUDE_FROM_ALL tests/t_granny_device.c granny_android.c ${DM_HOST}/hle_granny.c)
target_compile_definitions(dm_android_granny_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_compile_options(dm_android_granny_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_link_libraries(dm_android_granny_probe PRIVATE dm_android_core)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vulkan_probe.c")
  add_executable(dm_android_vulkan_probe vulkan_probe.c)
  target_compile_options(dm_android_vulkan_probe PRIVATE -Wall -Wextra -Werror -ffixed-x18)
  target_link_libraries(dm_android_vulkan_probe PRIVATE vulkan android log m)
  target_link_options(dm_android_vulkan_probe PRIVATE -Wl,-z,max-page-size=16384)
endif()
# Device positive controls for format-aware sentinels, mip/layer isolation,
# one readback per pass, and the resolution/world budgets.
add_executable(dm_android_vk_coverage_probe EXCLUDE_FROM_ALL tests/t_vk_coverage_device.c)
target_include_directories(dm_android_vk_coverage_probe PRIVATE ../host . ../gfx/nvn ../gfx/nvn/include)
target_compile_options(dm_android_vk_coverage_probe PRIVATE -Wall -Wextra -Werror -Wno-unused-function -ffixed-x18)
target_compile_definitions(dm_android_vk_coverage_probe PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0)
target_link_libraries(dm_android_vk_coverage_probe PRIVATE vulkan log m)
target_link_options(dm_android_vk_coverage_probe PRIVATE -Wl,-z,max-page-size=16384)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/romfs_image.c")
  target_sources(dm_android_core PRIVATE romfs_image.c)
  # romfs_samples.h is generated from a locally supplied game image. Keep the
  # app build reproducible without proprietary samples while allowing the
  # stronger readback probe to consume a generated header outside the checkout.
  set(DM_ANDROID_ROMFS_SAMPLES_HEADER "" CACHE FILEPATH
      "Generated romfs_samples.h for the optional original-resource probe")
  if(NOT DM_ANDROID_ROMFS_SAMPLES_HEADER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/romfs_samples.h")
    set(DM_ANDROID_ROMFS_SAMPLES_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/romfs_samples.h")
  endif()
  if(DM_ANDROID_ROMFS_SAMPLES_HEADER)
    if(NOT EXISTS "${DM_ANDROID_ROMFS_SAMPLES_HEADER}")
      message(FATAL_ERROR "DM_ANDROID_ROMFS_SAMPLES_HEADER does not exist: ${DM_ANDROID_ROMFS_SAMPLES_HEADER}")
    endif()
    get_filename_component(DM_ANDROID_ROMFS_SAMPLES_DIR
      "${DM_ANDROID_ROMFS_SAMPLES_HEADER}" DIRECTORY)
    set(DM_RESOURCE_PROBE_SOURCE game_resource_probe.c)
  else()
    set(DM_RESOURCE_PROBE_SOURCE game_resource_probe_stub.c)
  endif()
  # These helpers are the reviewed D2R 3.3.94012 title adapter.  Keep them
  # outside dm_android_core so another title cannot call D2R-private offsets.
  # d2r_host_modules.c attaches only the gates present in the loaded artifact.
  set(DM_ANDROID_VAULT_TABLE "" CACHE FILEPATH "Generated dm_vault_tab.h for the D2R UI bridge")
  option(DM_ANDROID_D2R_HOST_MODULES "Enable verified D2R 94012 private adapters" OFF)
  option(DM_ANDROID_DEBUG_CONSOLE "Build the opt-in D2R QA console (isolated saves)" OFF)
  if(DM_ANDROID_DEBUG_CONSOLE AND NOT DM_ANDROID_D2R_HOST_MODULES)
    message(FATAL_ERROR "The QA console requires DM_ANDROID_D2R_HOST_MODULES")
  endif()
  add_library(d2r_mobile_android SHARED app_jni.c d2r_mod_loader.c ${DM_RESOURCE_PROBE_SOURCE})
  if(DM_ANDROID_DEBUG_CONSOLE)
    target_sources(d2r_mobile_android PRIVATE d2r_debug_mode.c)
    target_compile_definitions(d2r_mobile_android PRIVATE DM_ANDROID_DEBUG_CONSOLE=1)
  endif()
  target_include_directories(d2r_mobile_android PRIVATE "${CMAKE_BINARY_DIR}/generated")
  if(DM_ANDROID_D2R_HOST_MODULES)
  if(NOT DM_ANDROID_VAULT_TABLE)
    message(FATAL_ERROR "D2R 94012 adapters require a matching DM_ANDROID_VAULT_TABLE")
  endif()
  target_sources(d2r_mobile_android PRIVATE d2r_haptics_android.c
    d2r_host_modules.c d2r_save_adapter.c granny_android.c ${DM_HOST}/hle_granny.c ${DM_HOST}/dm_textbox.c ${DM_HOST}/dm_textinput.c ${DM_HOST}/dm_turnsmooth.c
    ${DM_HOST}/dm_dropfilter.c ${DM_HOST}/dm_drophaptic.c ${DM_HOST}/dm_townwp.c ${DM_HOST}/hle_private_aim.c
    ${DM_HOST}/dm_rift.c ${DM_HOST}/dm_bossrush.c ${DM_HOST}/dm_rift_ui.c ${DM_HOST}/dm_padmode.c
    ../ios/Sources/dm_touch_game_state.c ../ios/Sources/dm_touch_game_identity.c
    ../ios/Sources/dm_touch_game_bridge.c)
  # Widget, item and world interaction use the same implementation as iOS.
  # The vault table is generated from the matching locally supplied RomFS;
  # never substitute an empty table or enable title-private UI for other games.
  if(DM_ANDROID_VAULT_TABLE)
    if(NOT EXISTS "${DM_ANDROID_VAULT_TABLE}")
      message(FATAL_ERROR "DM_ANDROID_VAULT_TABLE does not exist: ${DM_ANDROID_VAULT_TABLE}")
    endif()
    configure_file("${DM_ANDROID_VAULT_TABLE}" "${CMAKE_BINARY_DIR}/generated/dm_vault_tab.h" COPYONLY)
    target_sources(d2r_mobile_android PRIVATE d2r_ui_adapter.c d2r_hud_android.c d2r_custom_android.c d2r_world_overlay_android.c d2r_map_reveal.c
      ${DM_HOST}/dm_update.c
      ${DM_HOST}/dm_cursor.c ${DM_HOST}/dm_actions.c ${DM_HOST}/dm_world.c
      ${DM_HOST}/dm_cursor_item.c ${DM_HOST}/dm_uiscreen.c ${DM_HOST}/dm_prof.c
      ${DM_HOST}/dm_beltauto.c ${DM_HOST}/dm_elysium.c ${DM_HOST}/dm_freetp.c
      ${DM_HOST}/dm_pages.c ${DM_HOST}/dm_potpick.c ${DM_HOST}/dm_pouch.c
      ${DM_HOST}/dm_vault.c ${DM_HOST}/dm_vault_ui.c ${DM_HOST}/dm_kills.c
      ${DM_HOST}/dm_runeword.c ${DM_HOST}/dm_lootfilter.c ${DM_HOST}/dm_autobuff.c
      ${DM_HOST}/dm_shoprefresh.c)
    target_sources(d2r_mobile_android PRIVATE d2r_touch_flow_android.c d2r_touch_skill_flow_android.c d2r_touch_shell_android.c)
    target_compile_definitions(d2r_mobile_android PRIVATE DM_ANDROID_D2R_UI=1)
    message(STATUS "Android D2R UI bridge enabled with ${DM_ANDROID_VAULT_TABLE}")
    set(DM_ANDROID_NATIVE_ART_SOURCE "" CACHE FILEPATH "Verified generated dm_touch_native_art.c for native Android skill glyphs")
    if(DM_ANDROID_NATIVE_ART_SOURCE)
      if(NOT EXISTS "${DM_ANDROID_NATIVE_ART_SOURCE}")
        message(FATAL_ERROR "DM_ANDROID_NATIVE_ART_SOURCE does not exist: ${DM_ANDROID_NATIVE_ART_SOURCE}")
      endif()
      target_sources(d2r_mobile_android PRIVATE "${DM_ANDROID_NATIVE_ART_SOURCE}")
      target_compile_definitions(d2r_mobile_android PRIVATE DM_ANDROID_NATIVE_ART=1)
    else()
      target_compile_definitions(d2r_mobile_android PRIVATE DM_ANDROID_NATIVE_ART=0)
      message(STATUS "Android native skill atlas disabled; original skill glyphs retained")
    endif()
  endif()
  # D2R-only native BLTE decoder; other titles do not receive private gates.
  target_sources(d2r_mobile_android PRIVATE ${DM_HOST}/hle_tact.c
    ../third_party/libdeflate/lib/deflate_decompress.c
    ../third_party/libdeflate/lib/zlib_decompress.c
    ../third_party/libdeflate/lib/adler32.c
    ../third_party/libdeflate/lib/utils.c
    ../third_party/libdeflate/lib/arm/cpu_features.c)
  set_source_files_properties(
    ../third_party/libdeflate/lib/deflate_decompress.c
    ../third_party/libdeflate/lib/zlib_decompress.c
    ../third_party/libdeflate/lib/adler32.c
    ../third_party/libdeflate/lib/utils.c
    ../third_party/libdeflate/lib/arm/cpu_features.c
    PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-Wno-sign-compare")
  set(DM_ANDROID_HUD_EXP_HEADER "" CACHE FILEPATH "Generated D2R experience table for Android HUD percentages")
  if(DM_ANDROID_HUD_EXP_HEADER)
    if(NOT EXISTS "${DM_ANDROID_HUD_EXP_HEADER}")
      message(FATAL_ERROR "DM_ANDROID_HUD_EXP_HEADER does not exist: ${DM_ANDROID_HUD_EXP_HEADER}")
    endif()
    file(TO_CMAKE_PATH "${DM_ANDROID_HUD_EXP_HEADER}" DM_ANDROID_HUD_EXP_INCLUDE)
    target_compile_definitions(d2r_mobile_android PRIVATE DM_ANDROID_HUD_EXP_HEADER="${DM_ANDROID_HUD_EXP_INCLUDE}")
    target_compile_definitions(dm_android_d2r_save_probe PRIVATE DM_ANDROID_HUD_EXP_HEADER="${DM_ANDROID_HUD_EXP_INCLUDE}")
  endif()
  if(DM_ANDROID_ROMFS_SAMPLES_HEADER)
    target_include_directories(d2r_mobile_android PRIVATE "${DM_ANDROID_ROMFS_SAMPLES_DIR}")
    message(STATUS "Android original-resource probe enabled with ${DM_ANDROID_ROMFS_SAMPLES_HEADER}")
  else()
    message(STATUS "Android original-resource probe disabled; set DM_ANDROID_ROMFS_SAMPLES_HEADER to enable it")
  endif()
  endif() # DM_ANDROID_D2R_HOST_MODULES
  target_compile_options(d2r_mobile_android PRIVATE -Wall -Wextra -Werror -Wno-format -Wno-unused-function -ffixed-x18)
  target_compile_definitions(d2r_mobile_android PRIVATE _GNU_SOURCE TARGET_OS_IPHONE=0 DM_ANDROID_HOST=1)
  target_link_libraries(d2r_mobile_android PRIVATE dm_android_core dm_android_gfx)
  target_link_options(d2r_mobile_android PRIVATE -Wl,-z,max-page-size=16384)
endif()

# Skills/stat gestures need the same generated glyph resolver as the HUD to
# exercise the real normal/alternate empty-slot policy. Never linked into APK.
if(DM_ANDROID_NATIVE_ART_SOURCE)
  add_executable(dm_android_d2r_skill_flow_probe EXCLUDE_FROM_ALL
    tests/t_d2r_touch_skill_flow_device.c d2r_touch_skill_flow_android.c d2r_touch_flow_android.c
    "${DM_ANDROID_NATIVE_ART_SOURCE}")
  target_compile_definitions(dm_android_d2r_skill_flow_probe PRIVATE
    _GNU_SOURCE TARGET_OS_IPHONE=0 DM_ANDROID_NATIVE_ART=1)
  target_compile_options(dm_android_d2r_skill_flow_probe PRIVATE
    -Wall -Wextra -Werror -Wno-format -ffixed-x18)
  target_link_libraries(dm_android_d2r_skill_flow_probe PRIVATE dm_android_core)
endif()
add_executable(dm_android_d2r_skill_focus_probe EXCLUDE_FROM_ALL tests/t_d2r_skill_focus_device.c)
target_compile_options(dm_android_d2r_skill_focus_probe PRIVATE -Wall -Wextra -Werror -Wno-format -ffixed-x18)
target_link_libraries(dm_android_d2r_skill_focus_probe PRIVATE dl)
