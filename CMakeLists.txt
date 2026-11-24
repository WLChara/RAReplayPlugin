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

add_library(ReplayCore STATIC src/core/encoding/Utf8.cpp)
target_include_directories(ReplayCore PUBLIC src/core)

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

# 回放统计与雷达事件的纯逻辑：游戏时间与整段时间轴（帧号 → 游戏时间）、战斗与建造统计、停产状态机、资金曲线、操作计数、超武释放判定、雷达事件历史与落点集合。不含引擎或平台依赖。
add_library(ReplayStats STATIC
    src/stats/EconomyHistory.cpp
    src/stats/GameClock.cpp
    src/stats/GameTimelineBuilder.cpp
    src/stats/MatchStatistics.cpp
    src/stats/OperationEvents.cpp
    src/stats/PlayerStatistics.cpp
    src/stats/ProductionMonitor.cpp
    src/stats/RadarEventLog.cpp
    src/stats/SuperWeaponLaunch.cpp
    src/stats/SuperWeaponMarkTracker.cpp)
target_link_libraries(ReplayStats PUBLIC ReplayViewer)

add_library(ReplayUi STATIC
    src/ui/ReplayHud.cpp
    src/ui/ReplayHudCard.cpp
    src/ui/ReplayHudEconomy.cpp
    src/ui/ReplayHudEconomyChart.cpp
    src/ui/ReplayHudEconomyModel.cpp
    src/ui/ReplayHudEconomyWindow.cpp
    src/ui/ReplayHudFormat.cpp
    src/ui/ReplayHudLayout.cpp
    src/ui/ReplayHudMarkModel.cpp
    src/ui/ReplayHudFps.cpp
    src/ui/ReplayHudFrameControls.cpp
    src/ui/ReplayHudGroups.cpp
    src/ui/ReplayHudPlayers.cpp
    src/ui/ReplayHudProduction.cpp
    src/ui/ReplayHudRadarBar.cpp
    src/ui/ReplayHudRadarModel.cpp
    src/ui/ReplayHudScale.cpp
    src/ui/ReplayHudScaleSelector.cpp
    src/ui/ReplayHudSeekModel.cpp
    src/ui/ReplayHudSidebarPanel.cpp
    src/ui/ReplayHudStats.cpp
    src/ui/ReplayHudSuperWeaponMarks.cpp
    src/ui/ReplayHudUnits.cpp
    src/ui/ReplayHudZoomControls.cpp
    src/ui/ReplayHudZoomModel.cpp)
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
    src/render/OverlayPanelPresence.cpp
    src/render/WglOverlayContext.cpp)
target_link_libraries(ReplayRender PUBLIC ReplayUi ReplayViewer PRIVATE ReplayCore)
target_link_libraries(ReplayHudPreview PRIVATE ReplayRender)

add_library(ReplayDomain STATIC
    src/replay/FrameCodec.cpp
    src/replay/RecordedControlGroups.cpp
    src/replay/ReplaySession.cpp)
target_include_directories(ReplayDomain PUBLIC src/replay)

# zlib 1.3.1（许可见 third_party/zlib/LICENSE）：只编译原始 deflate 压缩与解压所需的源文件，不编译 gz*、compress、uncompr、infback。
# 只由 ReplayStorage 私有链接，zlib 头只能由 src/storage 包含（scripts/check-boundaries.ps1 检查）。
add_library(ReplayZlib STATIC
    third_party/zlib/adler32.c
    third_party/zlib/crc32.c
    third_party/zlib/deflate.c
    third_party/zlib/inffast.c
    third_party/zlib/inflate.c
    third_party/zlib/inftrees.c
    third_party/zlib/trees.c
    third_party/zlib/zutil.c)
target_include_directories(ReplayZlib PUBLIC third_party/zlib)
target_compile_options(ReplayZlib PRIVATE /W0)

add_library(ReplayStorage STATIC
    src/storage/ReplayArchive.cpp
    src/storage/ReplayBlockCodec.cpp
    src/storage/FrameReader.cpp
    src/storage/FrameWriter.cpp
    src/storage/ViewerControlProtocol.cpp
    src/storage/ViewerControlChannel.cpp
    src/storage/PluginSettings.cpp
    src/storage/ReplayConfig.cpp)
target_include_directories(ReplayStorage PUBLIC src/storage)
target_link_libraries(ReplayStorage PUBLIC ReplayDomain PRIVATE bcrypt ReplayZlib)

# 固定目标 gamemd.exe 的身份校验、必要地址与 ABI；TargetAbi.h 是纯头文件。
add_library(ReplayTarget STATIC
    src/target/TargetAbi.h
    src/target/TargetImage.cpp
    src/target/ViewerControlCallers.cpp)
target_link_libraries(ReplayTarget PRIVATE bcrypt)

# 不含引擎对象与平台调用的像素与缓存辅助：Cameo 像素解码、图标加载缓存、半黑幕帧像素缓存。
add_library(ReplayAssets STATIC
    src/assets/CameoPixels.cpp
    src/assets/ShroudPixels.cpp
    src/assets/IconLoadCache.cpp)
target_link_libraries(ReplayAssets PUBLIC ReplayViewer)

# 战局缩放的纯数学：物理战术区与逻辑视口的区间映射、档位与内存预算、锚点镜头、16 位像素缩放核。只依赖标准库。
add_library(ReplayZoom STATIC
    src/zoom/ScalePixels.cpp
    src/zoom/ZoomAnchor.cpp
    src/zoom/ZoomLevels.cpp
    src/zoom/ZoomMapping.cpp)

add_library(ReplayGameApi STATIC
    src/game_api/GameApi.cpp
    src/game_api/capture/DispatchedEvents.cpp
    src/game_api/capture/EventCaptureDiagnostics.cpp
    src/game_api/capture/DispatchEventCapture.cpp
    src/game_api/control_groups/ControlGroupRecorder.cpp
    src/game_api/control_groups/RecordedControlGroupProjection.cpp
    src/game_api/native/NativeControlGroups.cpp
    src/game_api/native/NativeObjectRedraw.cpp
    src/game_api/HouseRoster.cpp
    src/game_api/ReplayWorldReader.cpp
    src/game_api/stats/StatsReader.cpp
    src/game_api/NativeSidebar.cpp
    src/game_api/NativeTooltips.cpp
    src/game_api/names/NativeDisplayNames.cpp
    src/game_api/icons/CameoReader.cpp
    src/game_api/radar/NativeTacticalView.cpp
    src/game_api/radar/RadarEventResolver.cpp
    src/game_api/radar/RadarObservation.cpp
    src/game_api/PlayerInputEffects.cpp
    src/game_api/ViewerPlaybackSpeed.cpp
    src/game_api/native/NativeGame.cpp
    src/game_api/zoom/BattlefieldZoomAdapter.cpp
    src/game_api/zoom/ZoomFramePlan.cpp)
target_sources(ReplayGameApi PRIVATE src/game_api/native/NativeShroud.cpp)
target_include_directories(ReplayGameApi PRIVATE third_party/yrpp)
target_compile_definitions(ReplayGameApi PRIVATE SYR_VER=2)
target_link_libraries(ReplayGameApi PUBLIC ReplayDomain ReplayStats ReplayViewer ReplayTarget ReplayAssets ReplayZoom PRIVATE ReplayCore)

add_library(ReplayGraphicsHooks STATIC
    src/hooks/graphics/ReplayOverlayHook.cpp
    src/hooks/graphics/D3D9HookTargets.cpp
    src/hooks/graphics/OverlayWindowHook.cpp)
target_link_libraries(ReplayGraphicsHooks PUBLIC ReplayRender ReplayViewer ReplayMinHook)

# 隔离硬件宿主烟测，不启动 gamemd，不纳入无图形环境的常规 CTest。
add_executable(ReplayHudD3D9Smoke tools/ReplayHudD3D9Smoke.cpp tools/ReplayHudDemoData.cpp)
target_link_libraries(ReplayHudD3D9Smoke PRIVATE ReplayGraphicsHooks d3d9)

add_library(RAReplayPlugin SHARED
    src/hooks/AircraftShadowHooks.cpp
    src/hooks/BattlefieldZoomHooks.cpp
    src/hooks/ControlGroupHooks.cpp
    src/hooks/MovementFeedbackHooks.cpp
    src/hooks/ReplayHooks.cpp
    src/hooks/ReplayPauseHooks.cpp
    src/hooks/ReplayRadarHooks.cpp
    src/hooks/ReplayShroudHooks.cpp
    src/hooks/ReplaySidebarHooks.cpp
    src/hooks/ReplayInputHooks.cpp
    src/hooks/ReplaySpeedHooks.cpp
    src/hooks/ViewerInputHooks.cpp
    src/hooks/ViewerInputScope.cpp
    src/plugin/Entry.cpp
    src/plugin/ReplayRuntime.cpp
    src/plugin/PlaybackViewingSession.cpp
    src/plugin/BattlefieldZoomSession.cpp
    src/plugin/BattlefieldZoomViewMapping.cpp
    src/plugin/RuntimeGates.cpp
    src/plugin/ViewerCommandRouter.cpp
    src/plugin/PlaybackSeek.cpp
    src/plugin/PlaybackTimelineScan.cpp
    src/plugin/StatsDigest.cpp
    src/plugin/NativeSidebarGate.cpp
    src/plugin/NativeTooltipGate.cpp
    src/plugin/ReplayViewPublisher.cpp
    src/plugin/ReplayRadarSession.cpp
    src/plugin/ReplayStatsSession.cpp
    src/plugin/OverlayDiagnosticsReporter.cpp
    src/plugin/EventCaptureReporter.cpp
    src/plugin/Diagnostics.cpp)
target_include_directories(RAReplayPlugin PRIVATE third_party/yrpp)
target_compile_definitions(RAReplayPlugin PRIVATE SYR_VER=2)
target_link_libraries(RAReplayPlugin PRIVATE ReplayGameApi ReplayStats ReplayStorage ReplayDomain ReplayViewer ReplayZoom ReplayTarget ReplayGraphicsHooks)
set_target_properties(RAReplayPlugin PROPERTIES PREFIX "" OUTPUT_NAME "RAReplayPlugin")

# 手动宿主烟测；不加入依赖本机游戏文件的常规 CTest。
add_executable(SyringeHandshakeTests tests/SyringeHandshakeTests.cpp)
target_include_directories(SyringeHandshakeTests PRIVATE third_party/yrpp)
set_target_properties(SyringeHandshakeTests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/handshake-smoke"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/build/handshake-smoke")

# 由脚本先生成 C# 测试数据，再运行离线协议贯通；不注册为普通 CTest。
add_executable(ReplayProtocolBridge tests/ProtocolBridge.cpp)
target_link_libraries(ReplayProtocolBridge PRIVATE ReplayStorage)

include(CTest)
if(BUILD_TESTING)
    add_executable(Utf8Tests tests/Utf8Tests.cpp)
    target_link_libraries(Utf8Tests PRIVATE ReplayCore)
    add_test(NAME Utf8 COMMAND Utf8Tests)
    add_executable(ShroudPixelsTests tests/ShroudPixelsTests.cpp)
    target_link_libraries(ShroudPixelsTests PRIVATE ReplayAssets)
    add_test(NAME ShroudPixels COMMAND ShroudPixelsTests)
    add_executable(PluginSettingsTests tests/PluginSettingsTests.cpp)
    target_link_libraries(PluginSettingsTests PRIVATE ReplayStorage)
    add_test(NAME PluginSettings COMMAND PluginSettingsTests)
    add_executable(ViewerControlChannelTests tests/ViewerControlChannelTests.cpp)
    target_link_libraries(ViewerControlChannelTests PRIVATE ReplayStorage)
    add_test(NAME ViewerControlChannel COMMAND ViewerControlChannelTests "${CMAKE_SOURCE_DIR}/tests/fixtures")
    add_executable(CameoAssetsTests tests/CameoAssetsTests.cpp)
    target_link_libraries(CameoAssetsTests PRIVATE ReplayAssets)
    add_test(NAME CameoAssets COMMAND CameoAssetsTests)

    # 战局缩放纯逻辑；ZoomPerformance 只输出 2392×1408 战术区各档的每帧耗时，耗时不作为失败条件。
    add_executable(ZoomTests tests/ZoomTests.cpp)
    target_link_libraries(ZoomTests PRIVATE ReplayZoom)
    add_test(NAME Zoom COMMAND ZoomTests)
    add_test(NAME ZoomPerformance COMMAND ZoomTests --performance)

    add_executable(IconTextureBudgetTests tests/IconTextureBudgetTests.cpp)
    add_test(NAME IconTextureBudget COMMAND IconTextureBudgetTests)

    add_executable(OverlayCursorTests tests/OverlayCursorTests.cpp)
    target_link_libraries(OverlayCursorTests PRIVATE ReplayRender)
    add_test(NAME OverlayCursor COMMAND OverlayCursorTests)

    add_executable(ReplayHudLayoutTests tests/ReplayHudLayoutTests.cpp)
    target_link_libraries(ReplayHudLayoutTests PRIVATE ReplayUi)
    add_test(NAME ReplayHudLayout COMMAND ReplayHudLayoutTests)

    add_executable(OverlayInputQueueTests tests/OverlayInputQueueTests.cpp)
    target_link_libraries(OverlayInputQueueTests PRIVATE ReplayRender user32)
    add_test(NAME OverlayInputQueue COMMAND OverlayInputQueueTests)

    # 无图形设备的 ImGui 帧：面板输入区域、心跳报告与窄面板溢出；静态库只取用被引用的 render 目标文件。
    add_executable(ReplayHudPanelTests tests/ReplayHudPanelTests.cpp)
    target_link_libraries(ReplayHudPanelTests PRIVATE ReplayRender user32)
    add_test(NAME ReplayHudPanel COMMAND ReplayHudPanelTests)

    # plugin 没有独立的静态库（源文件直接编进 RAReplayPlugin），以下八项 plugin 逻辑（两个原生门控、
    # 运行时门控、观看命令路由、快进状态、缩放状态换算、统计摘要与时间表扫描）的测试仍直接编译其源文件；运行时门控、命令路由与
    # 快进状态只用到 replay、viewer 头文件中的值类型，无需链接对应库；缩放状态换算只用到缩放适配器头文件中不含 YRpp 的状态类型与
    # ReplayZoom 的档位表；统计摘要链接 ReplayStats；时间表扫描（含工作线程）链接 ReplayStorage 与 ReplayStats，用临时目录里写出的
    # 回放验证，不涉及游戏。
    add_executable(NativeSidebarGateTests tests/NativeSidebarGateTests.cpp src/plugin/NativeSidebarGate.cpp)
    add_test(NAME NativeSidebarGate COMMAND NativeSidebarGateTests)

    add_executable(NativeTooltipGateTests tests/NativeTooltipGateTests.cpp src/plugin/NativeTooltipGate.cpp)
    add_test(NAME NativeTooltipGate COMMAND NativeTooltipGateTests)

    add_executable(RuntimeGatesTests tests/RuntimeGatesTests.cpp src/plugin/RuntimeGates.cpp)
    add_test(NAME RuntimeGates COMMAND RuntimeGatesTests)

    add_executable(ViewerCommandRouterTests tests/ViewerCommandRouterTests.cpp src/plugin/ViewerCommandRouter.cpp)
    add_test(NAME ViewerCommandRouter COMMAND ViewerCommandRouterTests)

    add_executable(PlaybackSeekTests tests/PlaybackSeekTests.cpp src/plugin/PlaybackSeek.cpp)
    add_test(NAME PlaybackSeek COMMAND PlaybackSeekTests)

    add_executable(StatsDigestTests tests/StatsDigestTests.cpp src/plugin/StatsDigest.cpp)
    target_link_libraries(StatsDigestTests PRIVATE ReplayStats)
    add_test(NAME StatsDigest COMMAND StatsDigestTests)

    add_executable(PlaybackTimelineScanTests tests/PlaybackTimelineScanTests.cpp src/plugin/PlaybackTimelineScan.cpp)
    target_link_libraries(PlaybackTimelineScanTests PRIVATE ReplayStorage ReplayStats)
    add_test(NAME PlaybackTimelineScan COMMAND PlaybackTimelineScanTests)

    add_executable(BattlefieldZoomViewTests tests/BattlefieldZoomViewTests.cpp src/plugin/BattlefieldZoomViewMapping.cpp)
    target_link_libraries(BattlefieldZoomViewTests PRIVATE ReplayZoom)
    add_test(NAME BattlefieldZoomView COMMAND BattlefieldZoomViewTests)

    add_executable(GameClockTests tests/GameClockTests.cpp)
    target_link_libraries(GameClockTests PRIVATE ReplayStats)
    add_test(NAME GameClock COMMAND GameClockTests)

    add_executable(GameTimelineBuilderTests tests/GameTimelineBuilderTests.cpp)
    target_link_libraries(GameTimelineBuilderTests PRIVATE ReplayStats)
    add_test(NAME GameTimelineBuilder COMMAND GameTimelineBuilderTests)

    add_executable(PlayerStatisticsTests tests/PlayerStatisticsTests.cpp)
    target_link_libraries(PlayerStatisticsTests PRIVATE ReplayStats)
    add_test(NAME PlayerStatistics COMMAND PlayerStatisticsTests)

    add_executable(ProductionMonitorTests tests/ProductionMonitorTests.cpp)
    target_link_libraries(ProductionMonitorTests PRIVATE ReplayStats)
    add_test(NAME ProductionMonitor COMMAND ProductionMonitorTests)

    add_executable(OperationEventsTests tests/OperationEventsTests.cpp)
    target_link_libraries(OperationEventsTests PRIVATE ReplayStats)
    add_test(NAME OperationEvents COMMAND OperationEventsTests)

    add_executable(EconomyHistoryTests tests/EconomyHistoryTests.cpp)
    target_link_libraries(EconomyHistoryTests PRIVATE ReplayStats)
    add_test(NAME EconomyHistory COMMAND EconomyHistoryTests)

    add_executable(MatchStatisticsTests tests/MatchStatisticsTests.cpp)
    target_link_libraries(MatchStatisticsTests PRIVATE ReplayStats)
    add_test(NAME MatchStatistics COMMAND MatchStatisticsTests)

    add_executable(SuperWeaponLaunchTests tests/SuperWeaponLaunchTests.cpp)
    target_link_libraries(SuperWeaponLaunchTests PRIVATE ReplayStats)
    add_test(NAME SuperWeaponLaunch COMMAND SuperWeaponLaunchTests)

    add_executable(RadarEventLogTests tests/RadarEventLogTests.cpp)
    target_link_libraries(RadarEventLogTests PRIVATE ReplayStats)
    add_test(NAME RadarEventLog COMMAND RadarEventLogTests)

    add_executable(ReplayHudRadarModelTests tests/ReplayHudRadarModelTests.cpp)
    target_link_libraries(ReplayHudRadarModelTests PRIVATE ReplayUi)
    add_test(NAME ReplayHudRadarModel COMMAND ReplayHudRadarModelTests)

    add_executable(ReplayHudFormatTests tests/ReplayHudFormatTests.cpp)
    target_link_libraries(ReplayHudFormatTests PRIVATE ReplayUi)
    add_test(NAME ReplayHudFormat COMMAND ReplayHudFormatTests)

    add_executable(ReplayHudEconomyModelTests tests/ReplayHudEconomyModelTests.cpp)
    target_link_libraries(ReplayHudEconomyModelTests PRIVATE ReplayUi)
    add_test(NAME ReplayHudEconomyModel COMMAND ReplayHudEconomyModelTests)

    add_executable(ReplayViewExchangeTests tests/ReplayViewExchangeTests.cpp)
    target_link_libraries(ReplayViewExchangeTests PRIVATE ReplayViewer)
    add_test(NAME ReplayViewExchange COMMAND ReplayViewExchangeTests)

    add_executable(FrameCodecTests tests/FrameCodecTests.cpp)
    target_link_libraries(FrameCodecTests PRIVATE ReplayDomain)
    add_test(NAME FrameCodec COMMAND FrameCodecTests)

    add_executable(RecordedControlGroupsTests tests/RecordedControlGroupsTests.cpp)
    target_link_libraries(RecordedControlGroupsTests PRIVATE ReplayDomain)
    add_test(NAME RecordedControlGroups COMMAND RecordedControlGroupsTests "${CMAKE_SOURCE_DIR}/tests/fixtures")

    # 以下三项只用到 game_api 中不含 YRpp 的纯逻辑；静态库只取用被引用的目标文件，测试不依赖 YRpp 或游戏进程。
    add_executable(ControlGroupCaptureTests tests/ControlGroupCaptureTests.cpp)
    target_link_libraries(ControlGroupCaptureTests PRIVATE ReplayGameApi)
    add_test(NAME ControlGroupCapture COMMAND ControlGroupCaptureTests)

    add_executable(DispatchedEventCaptureTests tests/DispatchedEventCaptureTests.cpp)
    target_link_libraries(DispatchedEventCaptureTests PRIVATE ReplayGameApi)
    add_test(NAME DispatchedEventCapture COMMAND DispatchedEventCaptureTests)

    # 战局缩放的帧决策同样是 game_api 中不含 YRpp 的纯逻辑（game_api/zoom/ZoomFramePlan）。
    add_executable(ZoomFramePlanTests tests/ZoomFramePlanTests.cpp)
    target_link_libraries(ZoomFramePlanTests PRIVATE ReplayGameApi)
    add_test(NAME ZoomFramePlan COMMAND ZoomFramePlanTests)

    add_executable(ReplaySessionTests tests/ReplaySessionTests.cpp)
    target_link_libraries(ReplaySessionTests PRIVATE ReplayDomain)
    add_test(NAME ReplaySession COMMAND ReplaySessionTests)

    # 块编解码的纯逻辑：往返、块头与载荷的各无效条件、CRC-32 测试向量。
    add_executable(ReplayBlockCodecTests tests/ReplayBlockCodecTests.cpp)
    target_link_libraries(ReplayBlockCodecTests PRIVATE ReplayStorage)
    add_test(NAME ReplayBlockCodec COMMAND ReplayBlockCodecTests)

    # 冻结的 v2 样例按文件 SHA-256 核对，改写块后的 v3 文件需要重算内容哈希，测试直接使用 bcrypt。
    add_executable(ReplayArchiveTests tests/ReplayArchiveTests.cpp)
    target_link_libraries(ReplayArchiveTests PRIVATE ReplayStorage bcrypt)
    add_test(NAME ReplayArchive COMMAND ReplayArchiveTests "${CMAKE_SOURCE_DIR}/tests/fixtures")

    add_executable(UnfinalizedReplayTests tests/UnfinalizedReplayTests.cpp)
    target_link_libraries(UnfinalizedReplayTests PRIVATE ReplayStorage)
    add_test(NAME UnfinalizedReplay COMMAND UnfinalizedReplayTests)

    add_executable(RuntimeRequestTests tests/RuntimeRequestTests.cpp)
    target_link_libraries(RuntimeRequestTests PRIVATE ReplayStorage)
    add_test(NAME RuntimeRequest COMMAND RuntimeRequestTests)

    add_executable(ViewerPlaybackSpeedTests tests/ViewerPlaybackSpeedTests.cpp)
    target_link_libraries(ViewerPlaybackSpeedTests PRIVATE ReplayGameApi)
    add_test(NAME ViewerPlaybackSpeed COMMAND ViewerPlaybackSpeedTests)

    # hooks 没有独立的静态库（源文件直接编进 RAReplayPlugin），ViewerInputScope 仍直接编译；调用者表来自 ReplayTarget。
    add_executable(ViewerInputScopeTests tests/ViewerInputScopeTests.cpp src/hooks/ViewerInputScope.cpp)
    target_link_libraries(ViewerInputScopeTests PRIVATE ReplayTarget)
    add_test(NAME ViewerInputScope COMMAND ViewerInputScopeTests)
endif()
