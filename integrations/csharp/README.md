# C# 启动器接入

`RAReplay.Client` 是可独立编译的 .NET Framework 4.8 类库。三个协议类从 DTAClient 的回放模块抽取，保持原有二进制布局；类库不依赖 DTAClient、ClientCore、ClientGUI、XNA 或 Rampastring 程序集，也不包含完整客户端或 UI。公开命名空间为 `RAReplay.Client`。现有客户端若采用工程引用，须将原 `DTAClient.Domain.Replay` 引用改为该命名空间，避免同时编译两份协议实现。

录制时，宿主先按原流程写好最终 `spawn.ini` 和 `spawnmap.ini`，然后调用 `ReplayClient.PrepareRecording(gamePath, baseMapPath, mapName)`。返回的 `RecordingSession` 包含 `ReplayPath` 与 `Manifest`，并拥有写到游戏目录的 `rareplay-runtime.bin` 请求。宿主随后调用自己已有的游戏启动方法。启动失败或游戏进程真正退出时调用 `Dispose`；不要仅因启动方法返回就释放。释放会清理尚未由插件消费的本次请求，不会删除已生成的 `.yrrep`。

播放时，宿主选择 `.yrrep` 后调用 `ReplayClient.PreparePlayback(gamePath, replayPath, gameExecutablePath, resolveBaseMap)`。库校验完整容器、帧流、`gamemd.exe` 哈希及宿主解析的地图哈希，然后暂存原 `spawn.ini`、`spawnmap.ini`，写入录像保存的开局配置与播放请求。地图解析委托可使用 `manifest.FindBaseMap(gamePath)`；原有开局清单要求地图位于游戏目录内，库也会核对地图哈希。`PlaybackSession` 持有恢复责任，游戏退出或启动失败后必须 `Dispose`，包括准备前原文件不存在的情况。准备中途失败也会尝试回滚；若回滚同时失败，异常中保留两项错误。

宿主须保证准备调用与游戏启动串行，且游戏目录没有正在运行的游戏或其他活跃回放会话。每次只保留一个会话对象，并将其释放挂到宿主原有的进程退出及启动失败路径。恢复错误必须报告给调用方，并保留会话对象以便重试；启动与清理同时失败时应保留两项错误。库不订阅宿主事件，也不启动游戏。`HostExample` 是可编译的无 UI 回调示例，不会启动真实游戏。

运行离线验证：

播放会话同时拥有 `rareplay-viewer-<请求ID原始16字节的小写十六进制>.bin`，仅启动前原子创建，禁止覆盖。RPVC 固定为 32 字节小端：魔数 `0x43565052`、版本 `1`、动作（`0` Ready、`1` Restart）、选项（仅 bit0 为 50% 黑幕）、16 字节 RequestId。首次播放选项为 `0`，具备 UI dispatcher 和完整退出恢复逻辑的宿主显式调用 `PreparePlaybackWithRestart`，重播通过可选 `viewerFlags` 传递。原四参数 `PreparePlayback` 的真实方法签名保留，且不会发布 Ready，避免旧编译宿主广告无法处理的重播能力；缺少 dispatcher 时同样使用旧入口。

游戏真正退出后先调用 `PlaybackSession.TryConsumeRestart(out flags)`，只接受当前请求一次，再成功 `Dispose()` 恢复启动文件及删除自有请求、查看器文件。只有恢复成功才能用宿主现有 dispatcher 排回 UI 线程，以会话自己保存的录像路径新建播放会话并调用原有启动方法；不得从退出工作线程直接启动。普通退出、启动失败、非法或旧请求不重播；清理失败保留会话、报告错误、取消重播，重试恢复也不补发重播。文件不携带路径，插件不能指定重开的录像。库不改 RPRT V2、`.yrrep` 或 RPF2，不实现暂停或管理进程。

```powershell
dotnet build integrations/csharp/RAReplay.Client/RAReplay.Client.csproj
dotnet run --project integrations/csharp/RAReplay.Client.Tests/RAReplay.Client.Tests.csproj
dotnet build integrations/csharp/HostExample/HostExample.csproj
```

测试在临时目录使用合成地图、可执行文件字节和回放容器，不包含真实地图、录像、客户端配置或游戏二进制。当前接入只覆盖既有录制与离线播放格式，不提供浏览 UI、游戏启动器或独立命令行播放入口。
