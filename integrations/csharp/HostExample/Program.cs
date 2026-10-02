using System;
using RAReplay.Client;

// 仅演示宿主已有的启动与退出回调；此程序不会启动游戏。
internal sealed class HostReplayAdapter
{
    private readonly Action startGameProcess;
    private readonly Action<Action> queueOnUiThread;
    private readonly Func<bool> isGameRunning;
    private IDisposable activeSession;
    private string playbackRoot;
    private string playbackExecutable;
    private bool exitHandled;

    public HostReplayAdapter(Action startGameProcess, Action<Action> queueOnUiThread, Func<bool> isGameRunning = null)
    {
        this.startGameProcess = startGameProcess;
        this.queueOnUiThread = queueOnUiThread;
        this.isGameRunning = isGameRunning ?? (() =>
            System.Diagnostics.Process.GetProcessesByName("gamemd").Length != 0 ||
            System.Diagnostics.Process.GetProcessesByName("Syringe").Length != 0);
    }

    public void OnLaunchFilesReady(string gamePath, string baseMapPath, string mapName)
    {
        if (activeSession != null) throw new InvalidOperationException("已有活跃回放会话。");
        activeSession = ReplayClient.PrepareRecording(gamePath, baseMapPath, mapName);
        StartPreparedGame();
    }

    public void OnReplaySelected(string gamePath, string replayPath, string gameExecutablePath, uint viewerFlags = 0)
    {
        if (activeSession != null) throw new InvalidOperationException("已有活跃回放会话。");
        activeSession = queueOnUiThread == null ?
            ReplayClient.PreparePlayback(gamePath, replayPath, gameExecutablePath, manifest => manifest.FindBaseMap(gamePath)) :
            ReplayClient.PreparePlaybackWithRestart(gamePath, replayPath, gameExecutablePath,
                manifest => manifest.FindBaseMap(gamePath), viewerFlags);
        playbackRoot = gamePath;
        playbackExecutable = gameExecutablePath;
        exitHandled = false;
        StartPreparedGame();
    }

    private void StartPreparedGame()
    {
        try { startGameProcess(); }
        catch (Exception startError)
        {
            try { Cleanup(false); }
            catch (Exception restoreError)
            {
                throw new AggregateException("启动失败，且回放清理失败。",
                    startError, restoreError);
            }
            throw;
        }
        // 启动调用返回不代表游戏已退出；保留会话供真正的退出回调清理。
    }

    public void OnGameProcessExited()
    {
        Cleanup(true);
    }

    private void Cleanup(bool allowRestart)
    {
        IDisposable session = activeSession;
        if (session == null) return;
        PlaybackSession playback = session as PlaybackSession;
        uint viewerFlags = 0;
        bool restart = false;
        bool readRestart = allowRestart && !exitHandled && playback != null;
        exitHandled = true;
        try { restart = readRestart && playback.TryConsumeRestart(out viewerFlags); }
        catch (Exception error) { Console.Error.WriteLine("读取重播意图失败，取消自动重播：" + error.Message); }
        string replayPath = playback == null ? null : playback.ReplayPath;
        string gamePath = playbackRoot;
        string executable = playbackExecutable;
        // Dispose 异常时不清空所有者、不排队；恢复重试也不能补发已消费的重播。
        session.Dispose();
        activeSession = null;
        if (restart)
            queueOnUiThread(() =>
            {
                if (activeSession != null || isGameRunning()) return;
                OnReplaySelected(gamePath, replayPath, executable, viewerFlags);
            });
    }
}

#if !REPLAY_HOST_TESTS
internal static class Program
{
    private static void Main()
    {
        HostReplayAdapter example = new HostReplayAdapter(() =>
            Console.WriteLine("模拟现有宿主的 StartGameProcess 调用"), callback =>
            Console.WriteLine("模拟排回宿主 UI 线程，不执行真实启动"));
        Console.WriteLine("将 OnLaunchFilesReady、OnReplaySelected 和 OnGameProcessExited 接入现有宿主回调。");
    }
}
#endif
