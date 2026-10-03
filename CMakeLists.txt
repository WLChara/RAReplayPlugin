cmake_minimum_required(VERSION 3.20)
project(RAReplayPlugin VERSION 0.3.1 LANGUAGES C CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "RAReplayPlugin requires Windows and MSVC")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure with -A Win32: gamemd.exe and the plugin are x86")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8 /W4 /permissive-)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

# 仅使用 ImGui 核心和官方后端，不编译随库附带的旧 Hook 包装。
add_library(ReplayImGui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_impl_dx9.cpp
    third_party/imgui/imgui_impl_win32.cpp
    third_party/imgui/imgui_impl_opengl3.cpp)
target_include_directories(ReplayImGui PUBLIC third_party/imgui)
target_link_libraries(ReplayImGui PUBLIC opengl32 user32 gdi32 imm32 dwmapi)
target_compile_options(ReplayImGui PRIVATE /W0)

add_library(ReplayViewer STATIC src/viewer/ReplayViewExchange.cpp)
target_include_directories(ReplayViewer PUBLIC src/viewer)

add_library(ReplayUi STATIC
    src/ui/ReplayHud.cpp
    src/ui/ReplayHudLayout.cpp
    src/ui/ReplayHudFps.cpp
    src/ui/ReplayHudFrameControls.cpp
    src/ui/ReplayHudGroups.cpp
    src/ui/ReplayHudPlayers.cpp
    src/ui/ReplayHudProduction.cpp
    src/ui/ReplayHudUnits.cpp)
target_link_libraries(ReplayUi PUBLIC ReplayViewer ReplayImGui)

add_library(ReplayMinHook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c)
target_include_directories(ReplayMinHook PUBLIC third_party/minhook/include)
target_compile_options(ReplayMinHook PRIVATE /W0)

# 使用同一 UI 的开发预览；不注入或启动游戏。
add_executable(ReplayHudPreview tools/ReplayHudPreview.cpp tools/ReplayHudDemoData.cpp)
target_link_libraries(ReplayHudPreview PRIVATE ReplayUi)

add_library(ReplayRender STATIC
    src/render/OverlayCursor.cpp
    src/render/D3D9OverlayRenderer.cpp
    src/render/D3D9IconTextureCache.cpp
    src/render/OverlayInputProcessing.cpp
    src/render/ReplayOverlayRenderer.cpp
    src/render/OverlayFonts.cpp
    src/render/IconTextureCache.cpp
    src/render/OverlayInputQueue.cpp
    src/render/WglOverlayContext.cpp)
target_link_libraries(ReplayRender PUBLIC ReplayUi)
target_link_libraries(ReplayHudPreview PRIVATE ReplayRender)

add_library(ReplayDomain STATIC
    src/replay/FrameCodec.cpp
    src/replay/RecordedControlGroups.cpp
    src/replay/ReplaySession.cpp)
target_include_directories(ReplayDomain PUBLIC src/replay)

add_library(ReplayStorage STATIC
    src/storage/ReplayArchive.cpp
    src/storage/FrameReader.cpp
    src/storage/FrameWriter.cpp
    src/storage/ViewerControlProtocol.cpp
    src/storage/ViewerControlChannel.cpp)
target_include_directories(ReplayStorage PUBLIC src/storage)
target_link_libraries(ReplayStorage PUBLIC ReplayDomain PRIVATE bcrypt)

add_library(ReplayGameApi STATIC
    src/assets/CameoPixels.cpp
    src/assets/ShroudPixels.cpp
    src/assets/IconLoadCache.cpp
    src/game_api/GameApi.cpp
    src/game_api/ReplayWorldReader.cpp
    src/game_api/icons/CameoReader.cpp
    src/game_api/PlayerInputEffects.cpp
    src/game_api/ViewerPlaybackSpeed.cpp
    src/game_api/native/NativeGame.cpp)
target_sources(ReplayGameApi PRIVATE src/game_api/native/NativeShroud.cpp)
target_include_directories(ReplayGameApi PRIVATE third_party/yrpp)
target_compile_definitions(ReplayGameApi PRIVATE SYR_VER=2)
target_link_libraries(ReplayGameApi PUBLIC ReplayDomain)

add_library(ReplayGraphicsHooks STATIC
    src/hooks/graphics/ReplayOverlayHook.cpp
    src/hooks/graphics/D3D9HookTargets.cpp
    src/hooks/graphics/OverlayWindowHook.cpp)
target_link_libraries(ReplayGraphicsHooks PUBLIC ReplayRender ReplayMinHook)

# 隔离硬件宿主烟测，不启动 gamemd，不纳入无图形环境的常规 CTest。
add_executable(ReplayHudD3D9Smoke tools/ReplayHudD3D9Smoke.cpp tools/ReplayHudDemoData.cpp)
target_link_libraries(ReplayHudD3D9Smoke PRIVATE ReplayGraphicsHooks d3d9)

add_library(RAReplayPlugin SHARED
    src/hooks/MovementFeedbackHooks.cpp
    src/hooks/ReplayHooks.cpp
    src/hooks/ReplayPauseHooks.cpp
    src/hooks/ReplayShroudHooks.cpp
    src/hooks/ReplayInputHooks.cpp
    src/hooks/ReplaySpeedHooks.cpp
    src/hooks/ViewerInputHooks.cpp
    src/hooks/ViewerInputScope.cpp
    src/plugin/Entry.cpp
    src/plugin/ReplayConfig.cpp
    src/plugin/ReplayRuntime.cpp
    src/plugin/ReplayViewPublisher.cpp
    src/plugin/OverlayDiagnosticsReporter.cpp
    src/plugin/Diagnostics.cpp
    src/target/TargetImage.cpp)
target_sources(RAReplayPlugin PRIVATE src/target/ViewerControlCallers.cpp)
target_include_directories(RAReplayPlugin PRIVATE third_party/yrpp)
target_compile_definitions(RAReplayPlugin PRIVATE SYR_VER=2)
target_link_libraries(RAReplayPlugin PRIVATE ReplayGameApi ReplayStorage ReplayViewer ReplayGraphicsHooks bcrypt)
set_target_properties(RAReplayPlugin PROPERTIES PREFIX "" OUTPUT_NAME "RAReplayPlugin")

# 手动宿主烟测；不加入依赖本机游戏文件的常规 CTest。
add_executable(SyringeHandshakeTests tests/SyringeHandshakeTests.cpp)
target_include_directories(SyringeHandshakeTests PRIVATE third_party/yrpp)
set_target_properties(SyringeHandshakeTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/handshake-smoke"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/build/handshake-smoke")

include(CTest)
if(BUILD_TESTING)
    add_executable(ShroudPixelsTests tests/ShroudPixelsTests.cpp src/assets/ShroudPixels.cpp)
    add_test(NAME ShroudPixels COMMAND ShroudPixelsTests)
    add_executable(ViewerControlChannelTests tests/ViewerControlChannelTests.cpp)
    target_link_libraries(ViewerControlChannelTests PRIVATE ReplayStorage)
    add_test(NAME ViewerControlChannel COMMAND ViewerControlChannelTests "${CMAKE_SOURCE_DIR}/tests/fixtures")
    add_executable(CameoAssetsTests tests/CameoAssetsTests.cpp
        src/assets/CameoPixels.cpp src/assets/IconLoadCache.cpp)
    add_test(NAME CameoAssets COMMAND CameoAssetsTests)

    add_executable(IconTextureBudgetTests tests/IconTextureBudgetTests.cpp)
    add_test(NAME IconTextureBudget COMMAND IconTextureBudgetTests)

    add_executable(OverlayCursorTests tests/OverlayCursorTests.cpp)
    target_link_libraries(OverlayCursorTests PRIVATE ReplayRender)
    add_test(NAME OverlayCursor COMMAND OverlayCursorTests)

    add_executable(ReplayHudLayoutTests tests/ReplayHudLayoutTests.cpp)
    target_link_libraries(ReplayHudLayoutTests PRIVATE ReplayUi)
    add_test(NAME ReplayHudLayout COMMAND ReplayHudLayoutTests)

    add_executable(OverlayInputQueueTests tests/OverlayInputQueueTests.cpp src/render/OverlayInputQueue.cpp)
    target_link_libraries(OverlayInputQueueTests PRIVATE ReplayUi user32)
    add_test(NAME OverlayInputQueue COMMAND OverlayInputQueueTests)

    add_executable(ReplayViewExchangeTests tests/ReplayViewExchangeTests.cpp)
    target_link_libraries(ReplayViewExchangeTests PRIVATE ReplayViewer)
    add_test(NAME ReplayViewExchange COMMAND ReplayViewExchangeTests)

    add_executable(FrameCodecTests tests/FrameCodecTests.cpp)
    target_link_libraries(FrameCodecTests PRIVATE ReplayDomain)
    add_test(NAME FrameCodec COMMAND FrameCodecTests)

    add_executable(RecordedControlGroupsTests tests/RecordedControlGroupsTests.cpp)
    target_link_libraries(RecordedControlGroupsTests PRIVATE ReplayDomain)
    add_test(NAME RecordedControlGroups COMMAND RecordedControlGroupsTests "${CMAKE_SOURCE_DIR}/tests/fixtures")

    add_executable(ReplaySessionTests tests/ReplaySessionTests.cpp)
    target_link_libraries(ReplaySessionTests PRIVATE ReplayDomain)
    add_test(NAME ReplaySession COMMAND ReplaySessionTests)

    add_executable(ReplayArchiveTests tests/ReplayArchiveTests.cpp)
    target_link_libraries(ReplayArchiveTests PRIVATE ReplayStorage)
    add_test(NAME ReplayArchive COMMAND ReplayArchiveTests)

    add_executable(RuntimeRequestTests tests/RuntimeRequestTests.cpp src/plugin/ReplayConfig.cpp)
    add_test(NAME RuntimeRequest COMMAND RuntimeRequestTests)

    add_executable(ViewerPlaybackSpeedTests tests/ViewerPlaybackSpeedTests.cpp
        src/game_api/ViewerPlaybackSpeed.cpp)
    add_test(NAME ViewerPlaybackSpeed COMMAND ViewerPlaybackSpeedTests)

    add_executable(ViewerInputScopeTests tests/ViewerInputScopeTests.cpp
        src/hooks/ViewerInputScope.cpp src/target/ViewerControlCallers.cpp)
    add_test(NAME ViewerInputScope COMMAND ViewerInputScopeTests)
endif()
