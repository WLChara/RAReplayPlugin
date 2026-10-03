# RAReplayPlugin

《尤里的复仇》对局录制与回放插件源码。此仓库包含 x86 游戏插件、离线测试以及接入现有 DTAClient 所需的独立 C# 代码；不包含完整客户端、游戏文件或已构建产物。

插件通过 Syringe 加载，目标为 SHA-256 `7cd005d263fde203d9c84548200a057a8df61d724da3c6bd1e521eeb61cd0747` 的 `gamemd.exe`（ImageBase `0x400000`）。客户端负责生成录制或播放请求、检查地图并恢复开局文件；插件负责采集和重演模拟帧、读写回放容器以及显示观看界面。C# 接入范围与接入步骤见 [integrations/csharp/README.md](integrations/csharp/README.md)。

## 构建与测试

需要 Windows、Visual Studio 2022 C++ 工作负载、Windows SDK、CMake 和 PowerShell。按 Win32/x86 与 C++23 构建：

```powershell
.\scripts\build.ps1 -Configuration Release
```

该脚本运行源码边界检查、构建及常规 CTest。插件产物位于 `build/v2/Release/RAReplayPlugin.dll`。C# 与 C++ 的文件协议贯通测试在接入文件齐备后运行：

```powershell
.\scripts\test-protocol-bridge.ps1
```

可选的 Syringe 握手测试需要调用者显式提供与上述哈希匹配的游戏可执行文件。测试在临时目录中使用该文件，不启动游戏：

```powershell
.\scripts\test-plugin-handshake.ps1 -GameExe 'D:\Games\Yuri\gamemd.exe'
```

## 范围与限制

播放时可暂停和继续；读完最后一个完整帧后自动暂停并保留当前画面。重新播放会结束当前游戏进程，恢复启动文件后由支持此能力的客户端新建播放会话和游戏进程，不在已结束的世界中回绕。

重新播放需要支持 RPVC 通道的插件与客户端。C# 宿主必须显式调用 `PreparePlaybackWithRestart`，消费当前请求的重播意图，在恢复成功后排回 UI 线程调用原启动方法。旧四参数 `PreparePlayback` 保留真实方法签名并仍可播放，但不发布重播能力，重新播放不可用。失败、旧请求、恢复未成功或已有新会话时不会自动重开。

`50% SHROUD` 仅调整现有黑幕的绘制透明度，不改变地图揭示状态、战场可见性或模拟规则。首次手动播放默认关闭，重新播放延续当前选择。

本地编队记录使用 RPF2 V3，记录录制者本地的 0～9 号编队。首帧保存完整快照，后续变化以完整快照替换；未变化时沿用前帧状态，空快照清空编队。运输中的有效成员保留，成员死亡或失去控制后更新记录。观看界面显示记录中的编队；观看者临时选择和镜头操作独立于录制编队，不会改写组历史。

新录像需要支持 V3 的插件及客户端。旧 V2 录像仍可播放，但没有编队历史，界面会显示未记录。外层 RAREPL2 V2 容器、RPRT V2 与 RPVC V1 协议保持不变。

本轮编队记录已通过离线编译、16 项 C++ 测试、C# 新旧帧流及真实 C++ 编码样本校验，原重播生命周期检查也已通过；编队操作、运输、死亡及失去控制等路径的实际游戏验收尚待人工完成。

回放容器为 `.yrrep`，未完成写入使用 `.partial`。观看界面使用 Direct3D 9 或 OpenGL 呈现回调；GDI 软件回退不绘制界面。图标读取已接入 PCX 优先、SHP 回退，新游标与图标已获此前实机运行反馈。录制 FPS 与逐玩家黑幕尚未完整接入。当前不提供时间轴跳转、倒退或世界快照恢复。已有用户运行验证覆盖遭遇战录制与短回放；这不代表所有游戏版本、对局长度和图形后端均已验证。

第三方依赖及其来源状态见 [third_party/README.md](third_party/README.md)。本仓库未为自有源码附加许可文件；公开可读的源码与授予再分发许可是两件不同的事。
