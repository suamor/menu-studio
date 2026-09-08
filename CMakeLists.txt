cmake_minimum_required(VERSION 3.21)

# "Menu Studio" - world stays paused in menus, the player's animation
# graph + HDT/SMP physics keep ticking. Skyrim SE 1.5.97 only.
# (Internal name renamed from MenuTimeBubble -> MenuStudio; the C++ MTB::
# namespace is kept.)
project(MenuStudio VERSION 1.1.6 LANGUAGES CXX)

# Some versions of MSVC collide in own Windows headers with oder min/max declarations
if (WIN32)
    add_compile_definitions(NOMINMAX)
endif()

set(COMMONLIB_SSE_FOLDER ${CMAKE_CURRENT_SOURCE_DIR}/extern/src/CommonLibSSE-NG CACHE PATH "CommonLibSSE-NG source")
add_subdirectory(${COMMONLIB_SSE_FOLDER} ${CMAKE_BINARY_DIR}/CommonLibSSE)
include(${COMMONLIB_DIR}/cmake/CommonLibSSE.cmake)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(safetyhook CONFIG REQUIRED)

# ⚠ A CRASH LOG THAT CANNOT NAME OUR BUILD COSTS A DIAGNOSIS. This puts the
# version into the DLL itself, so Crash Logger and Trainwreck print it beside
# our name like every other plugin. Generated from project() so it cannot
# drift from the other version files.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
        src/PCH.h
        src/plugin.cpp
        src/Offsets.h
        src/EquipSoundMute.h
        src/VersionCheck.h
        src/VersionCheck.cpp
        src/Backdrop.h
        src/Backdrop.cpp
        src/BackdropPolicy.h
        src/BackdropAnchorPolicy.h
        src/BackdropImage.h
        src/BackdropImage.cpp
        src/BackdropImagePolicy.h
        src/BackdropManifest.h
        src/BackdropManifest.cpp
        src/BackdropPacks.h
        src/BackdropPacks.cpp
        src/CsCubemapBridge.h
        src/CsCubemapBridge.cpp
        src/FaceNeutral.h
        src/FaceNeutral.cpp
        src/AnimEventProbe.h
        src/AnimEventProbe.cpp
        src/ClipProgressPolicy.h
        src/ClipProbe.h
        src/CullWatch.cpp
        src/ClipProbe.cpp
        src/CompanionLunge.h
        src/CompanionLunge.cpp
        src/CompanionProbe.h
        src/CompanionProbe.cpp
        src/CompanionShotPolicy.h
        src/ActorTickProbe.h
        src/ActorTickProbe.cpp
        src/EquipNotifyGate.h
        src/EquipNotifyGate.cpp
        src/ForcePause.h
        src/ForcePause.cpp
        src/PauseLingerPolicy.h
        src/ShadowPause.h
        src/ShadowPause.cpp
        src/Settings.h
        src/Settings.cpp
        src/SettingsUI.h
        src/SettingsUI.cpp
        src/ActionBar.h
        src/ActionBar.cpp
        src/ChamferPolicy.h
        src/ChamferPanel.h
        src/ChamferPanel.cpp
        src/FrameArt.h
        src/FrameArt.cpp
        src/StudioCameraPolicy.h
        src/MountedSubjectPolicy.h
        src/StudioCamera.h
        src/StudioCamera.cpp
        src/CameraCloseProbePolicy.h
        src/CameraCloseProbe.h
        src/CameraCloseProbe.cpp
        src/SceneTint.h
        src/SceneTint.cpp
        src/StudioLight.h
        src/StudioLight.cpp
        src/StudioRig.h
        src/StudioRig.cpp
        src/Transition.h
        src/Transition.cpp
        src/RotationOwnershipPolicy.h
        src/MovementArmPolicy.h
        src/WeaponPumpPolicy.h
        src/MenuAnimationHoldPolicy.h
        src/ExternalViewExitPolicy.h
        src/StudioSessionPolicy.h
        src/ItemPreviewPolicy.h
        src/ItemPreviewBroker.h
        src/ItemPreviewBroker.cpp
        src/Bubble.h
        src/Bubble.cpp
        src/CameraGate.h
        src/CameraGate.cpp
        src/CameraCollisionPolicy.h
        src/CbpcDrive.h
        src/CbpcDrive.cpp
        src/FootIkGate.h
        src/FootIkGate.cpp
        src/Declutter.h
        src/Declutter.cpp
        src/PublicApi.cpp
        src/DeclutterRefPolicy.h
        src/FsmpDrive.h
        src/FsmpDrive.cpp
        src/MenuInputGate.h
        src/MenuInputGate.cpp
        src/OwnView.h
        src/OwnView.cpp
        src/WeaponPreviewGate.h
        src/WeaponPreview.h
        src/WeaponPreview.cpp
        src/WeaponStance.h)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE extern)

# FLICK / FUCK (Fuzz's Legally Intelligible Core Kit) is consumed at runtime via
# its C-ABI (extern/FUCK_API.h + FUCK.dll's RequestFUCK export); we only need
# imgui's headers/types for the API surface - FUCK.dll does the rendering.
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui safetyhook::safetyhook)
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

# A build for one reporter, not a build anybody installs on purpose. It forces
# bVerboseLog and bCameraCloseProbe on in memory whatever the player's INI says
# (their INI would otherwise win, and a reporter who has to edit two keys by hand
# is a reporter who sends back the wrong log), stamps the version string so any
# log it produces identifies itself, and turns on the extra close-side line that
# prints the reading's context against the live values.
#
# ⛔ NEVER SHIP ONE. tools/make_fomod.sh refuses to name a diagnostic build as
# the release, the same way it refuses a zip built with placeholder screenshots.
option(MENUSTUDIO_DIAG "Diagnostic build: force logging on, add the capture context line" OFF)
if(MENUSTUDIO_DIAG)
    target_compile_definitions(${PROJECT_NAME} PRIVATE MENUSTUDIO_DIAG)
    message(STATUS "MenuStudio: DIAGNOSTIC build, logging forced on. Do not release this.")
endif()

if(NOT DEFINED OUTPUT_FOLDER)
    set(OUTPUT_FOLDER "C:/Games/Nolvus/Instances/Nolvus Awakening/MODS/mods/Menu Studio")
endif()
set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
message(STATUS "MenuStudio deploy folder: ${DLL_FOLDER}")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/dist" "${OUTPUT_FOLDER}"
    VERBATIM)

# Optional standalone tests for the engine-free pieces - backdrop parsing,
# weapon preview decisions, equip-sound muting, movement arms and backdrop anchoring. Configure with
# -DMENUSTUDIO_TESTS=ON to build them; they are never part of the plugin.
# NOTE: vcpkg's simpleini port is header-only and ships no CMake config package
# (no unofficial::simpleini::simpleini target to find_package() against) - confirmed
# by inspecting its installed file list, which contains only headers + copyright/spdx
# metadata under share/simpleini. So the backdrop test adds its vcpkg include dir
# directly instead.
option(MENUSTUDIO_TESTS "Build the standalone engine-free tests" OFF)
if(MENUSTUDIO_TESTS)
    add_executable(backdrop_manifest_test
        tests/backdrop_manifest_test.cpp
        src/BackdropManifest.cpp)
    target_include_directories(backdrop_manifest_test PRIVATE src)
    target_include_directories(backdrop_manifest_test PRIVATE "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows-static-md/include")
    target_compile_features(backdrop_manifest_test PRIVATE cxx_std_23)

    add_executable(backdrop_image_policy_test
        tests/backdrop_image_policy_test.cpp)
    target_include_directories(backdrop_image_policy_test PRIVATE src)
    target_compile_features(backdrop_image_policy_test PRIVATE cxx_std_23)

    add_executable(weapon_preview_gate_test
        tests/weapon_preview_gate_test.cpp)
    target_include_directories(weapon_preview_gate_test PRIVATE src)
    target_compile_features(weapon_preview_gate_test PRIVATE cxx_std_23)

    add_executable(equip_sound_mute_test
        tests/equip_sound_mute_test.cpp)
    target_include_directories(equip_sound_mute_test PRIVATE src)
    target_compile_features(equip_sound_mute_test PRIVATE cxx_std_23)

    add_executable(movement_arm_policy_test
        tests/movement_arm_policy_test.cpp)
    target_include_directories(movement_arm_policy_test PRIVATE src)
    target_compile_features(movement_arm_policy_test PRIVATE cxx_std_23)

    add_executable(backdrop_anchor_policy_test
        tests/backdrop_anchor_policy_test.cpp)
    target_include_directories(backdrop_anchor_policy_test PRIVATE src)
    target_compile_features(backdrop_anchor_policy_test PRIVATE cxx_std_23)

    add_executable(camera_collision_policy_test
        tests/camera_collision_policy_test.cpp)
    target_include_directories(camera_collision_policy_test PRIVATE src)
    target_compile_features(camera_collision_policy_test PRIVATE cxx_std_23)

    add_executable(weapon_pump_policy_test
        tests/weapon_pump_policy_test.cpp)
    target_include_directories(weapon_pump_policy_test PRIVATE src)
    target_compile_features(weapon_pump_policy_test PRIVATE cxx_std_23)

    add_executable(clip_progress_policy_test
        tests/clip_progress_policy_test.cpp)
    target_include_directories(clip_progress_policy_test PRIVATE src)
    target_compile_features(clip_progress_policy_test PRIVATE cxx_std_23)

    add_executable(menu_animation_hold_policy_test
        tests/menu_animation_hold_policy_test.cpp)
    target_include_directories(menu_animation_hold_policy_test PRIVATE src)
    target_compile_features(menu_animation_hold_policy_test PRIVATE cxx_std_23)

    add_executable(pause_linger_policy_test
        tests/pause_linger_policy_test.cpp)
    target_include_directories(pause_linger_policy_test PRIVATE src)
    target_compile_features(pause_linger_policy_test PRIVATE cxx_std_23)

    add_executable(rotation_ownership_policy_test
        tests/rotation_ownership_policy_test.cpp)
    target_include_directories(rotation_ownership_policy_test PRIVATE src)
    target_compile_features(rotation_ownership_policy_test PRIVATE cxx_std_23)

    add_executable(external_view_exit_policy_test
        tests/external_view_exit_policy_test.cpp)
    target_include_directories(external_view_exit_policy_test PRIVATE src)
    target_compile_features(external_view_exit_policy_test PRIVATE cxx_std_23)

    add_executable(declutter_ref_policy_test
        tests/declutter_ref_policy_test.cpp)
    target_include_directories(declutter_ref_policy_test PRIVATE src)
    target_compile_features(declutter_ref_policy_test PRIVATE cxx_std_23)

    add_executable(camera_close_probe_policy_test
        tests/camera_close_probe_policy_test.cpp)
    target_include_directories(camera_close_probe_policy_test PRIVATE src)
    target_compile_features(camera_close_probe_policy_test PRIVATE cxx_std_23)

    add_executable(studio_camera_policy_test
        tests/studio_camera_policy_test.cpp)
    target_include_directories(studio_camera_policy_test PRIVATE src)
    target_compile_features(studio_camera_policy_test PRIVATE cxx_std_23)

    add_executable(camera_arm_state_policy_test
        tests/camera_arm_state_policy_test.cpp)
    target_include_directories(camera_arm_state_policy_test PRIVATE src)
    target_compile_features(camera_arm_state_policy_test PRIVATE cxx_std_23)

    add_executable(chamfer_policy_test
        tests/chamfer_policy_test.cpp)
    target_include_directories(chamfer_policy_test PRIVATE src)
    target_compile_features(chamfer_policy_test PRIVATE cxx_std_23)

    add_executable(mounted_subject_policy_test
        tests/mounted_subject_policy_test.cpp)
    target_include_directories(mounted_subject_policy_test PRIVATE src)
    target_compile_features(mounted_subject_policy_test PRIVATE cxx_std_23)

    add_executable(studio_session_policy_test
        tests/studio_session_policy_test.cpp)
    target_include_directories(studio_session_policy_test PRIVATE src)
    target_compile_features(studio_session_policy_test PRIVATE cxx_std_23)

    add_executable(item_preview_policy_test
        tests/item_preview_policy_test.cpp)
    target_include_directories(item_preview_policy_test PRIVATE src)
    target_compile_features(item_preview_policy_test PRIVATE cxx_std_23)

    add_executable(meter_policy_test
        tests/meter_policy_test.cpp)
    target_include_directories(meter_policy_test PRIVATE src)
    target_compile_features(meter_policy_test PRIVATE cxx_std_23)

    add_executable(frame_policy_test
        tests/frame_policy_test.cpp)
    target_include_directories(frame_policy_test PRIVATE src)
    target_compile_features(frame_policy_test PRIVATE cxx_std_23)

    add_executable(sun_park_policy_test
        tests/sun_park_policy_test.cpp)
    target_include_directories(sun_park_policy_test PRIVATE src)
    target_compile_features(sun_park_policy_test PRIVATE cxx_std_23)

    add_executable(path_cull_policy_test
        tests/path_cull_policy_test.cpp)
    target_include_directories(path_cull_policy_test PRIVATE src)
    target_compile_features(path_cull_policy_test PRIVATE cxx_std_23)

    add_executable(camera_debt_policy_test
        tests/camera_debt_policy_test.cpp)
    target_include_directories(camera_debt_policy_test PRIVATE src)
    target_compile_features(camera_debt_policy_test PRIVATE cxx_std_23)
endif()
