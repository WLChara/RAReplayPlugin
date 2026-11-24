#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rareplay::viewer
{
enum class UnitCategory { Infantry, Vehicle, Aircraft, Building, Other };

/** 图像是独立的 RGBA 像素副本；不持有游戏对象或图形设备指针。 */
struct IconImage
{
    std::string mKey;
    int mWidth = 0;
    int mHeight = 0;
    std::vector<std::uint8_t> mRgba;
};

struct UnitSummary
{
    std::string mTypeId;
    std::string mName;
    UnitCategory mCategory = UnitCategory::Other;
    std::uint32_t mCount = 0;
    std::shared_ptr<const IconImage> mIcon;
};

struct ProductionLine
{
    std::uint32_t mId = 0;
    std::string mLabel;
    std::string mProductName;
    std::shared_ptr<const IconImage> mIcon;
    float mProgress = 0.0f;
    bool mOnHold = false;
    bool mReady = false;
    std::uint32_t mQueuedCount = 0;
};

/**
 * 游戏时间：每完成一帧按该帧实际模拟速度档累加的毫秒数，观看者调速只改变播放快慢而不改变它。
 * 单机速度 0 在原生中不限速，该帧按 16 ms 名义值计；累加中出现过这种帧后整段时间只是名义时间。
 */
struct GameTime
{
    std::int64_t mMilliseconds = 0;
    bool mIsNominal = false;
};

/**
 * 整段回放的“帧号 → 游戏时间”表，口径与统计时钟（stats::GameClock）相同：第 L 项是第 L 帧完成后的累计游戏时间，
 * 即第 0 帧到第 L 帧时长之和（毫秒）。帧号与快照的 mFrameNumber 同一单位（最后完成的帧号）。
 * 生成后不可修改，以 shared_ptr<const GameTimeline> 在快照之间共享。
 */
struct GameTimeline
{
    /** 下标为帧号，元素数等于总帧数，单调不减。 */
    std::vector<std::int64_t> mCumulativeMs;
    /**
     * 从这一帧起（含）的时间只是名义值，与统计时钟一样一旦出现就延续到表尾；为空表示整表都不是名义时间。
     * 录像没有记录速度、按运行时速度整段换算时为 0。
     */
    std::optional<std::uint32_t> mFirstNominalFrame;

    std::uint32_t GetFrameCount() const noexcept { return static_cast<std::uint32_t>(mCumulativeMs.size()); }

    /** 第 frame 帧完成后的游戏时间；帧号超出表的范围时为空。 */
    std::optional<GameTime> GetTimeAt(std::uint32_t frame) const noexcept
    {
        if (frame >= mCumulativeMs.size())
            return std::nullopt;
        return GameTime{mCumulativeMs[frame], mFirstNominalFrame.has_value() && frame >= *mFirstNominalFrame};
    }
};

/** 停产状况只关注兵营（步兵产线）与重工（陆地载具产线，不含船厂）。 */
enum class ProductionLineKind { Barracks, WarFactory };

enum class ProductionLineState
{
    /** 没有可用的对应工厂（不含建造中的工厂）；不计停产。 */
    NoFactory,
    Producing,
    /** 玩家手动暂停；不算停产。 */
    Paused,
    /** 因没有通电的可用工厂而被自动挂起。 */
    PowerLoss,
    /** 资金不足，进度回退。 */
    NoFunds,
    /** 有工厂但没有任何项目在生产。 */
    Stalled
};

/** 一段已结束的停产；时间为游戏时间毫秒，mEndMs 不小于 mStartMs。 */
struct StallRecord
{
    std::int64_t mStartMs = 0;
    std::int64_t mEndMs = 0;
    /** 停产期间玩家所在房屋是否出现过电力不足。 */
    bool mHadPowerOutage = false;
};

/** 已结束的停产，按开始时间升序。快照之间共享，发布后不可修改。 */
using StallHistory = std::vector<StallRecord>;

struct ProductionLineStatus
{
    ProductionLineState mState = ProductionLineState::NoFactory;
    /** 仅 mState 为 Stalled 时有效：当前这段停产开始的游戏时间。 */
    std::int64_t mStalledSinceMs = 0;
    /** 仅 mState 为 Stalled 时有效：当前这段停产期间是否出现过电力不足。 */
    bool mStallHadPowerOutage = false;
    /** 空指针等同于没有历史。 */
    std::shared_ptr<const StallHistory> mHistory;
    /**
     * 总停产时间（毫秒）：已写入历史的停产区间时长之和，加上正在进行且已持续满 2 秒的停产时长；
     * 不足 2 秒的空档不计，与历史写入口径一致。
     */
    std::int64_t mTotalStalledMs = 0;
    /**
     * 停产率（百分数，0–100）：总停产时间 ÷ 当前游戏时间 × 100；游戏时间为 0 时为空。
     * 分母是整段游戏时间，含尚无对应工厂的时间；游戏时间为名义时间时该比率同样只是名义值。
     */
    std::optional<float> mStallRatePercent;
};

struct EconomyPoint
{
    std::int64_t mTimeMs = 0;
    std::int64_t mCredits = 0;
};

/** 降采样后的资金曲线，时间升序；快照之间共享，发布后不可修改。 */
struct EconomyCurve
{
    std::vector<EconomyPoint> mPoints;
};

/** 兵营或重工升级：渗透标志本身，以及它首次变为有效的游戏时间。 */
struct UpgradeStatus
{
    bool mIsActive = false;
    std::int64_t mActivatedAtMs = 0;
};

/** 摧毁与损失只计敌方（非本方、非盟友、非中立）。 */
struct CombatStatistics
{
    std::uint32_t mDestroyedUnits = 0;
    std::uint32_t mDestroyedBuildings = 0;
    std::uint32_t mLostUnits = 0;
    std::uint32_t mLostBuildings = 0;
    /** 摧毁总数 ÷ 损失总数；损失为 0 时为空。 */
    std::optional<float> mKillLossRatio;
};

struct PlayerStatistics
{
    /** 房屋下标超出击杀矩阵范围时为空。 */
    std::optional<CombatStatistics> mCombat;
    /** 工厂产出计数的合计，等于四个类别之和。 */
    std::uint32_t mBuiltBuildings = 0;
    std::uint32_t mBuiltVehicles = 0;
    std::uint32_t mBuiltInfantry = 0;
    std::uint32_t mBuiltAircraft = 0;
    std::uint32_t mScore = 0;
    /**
     * 录像中该玩家的操作数：同一帧内同类型、除执行者外参数相同的命令事件合并为一次（群体命令算一次）；
     * 非人类玩家（单机 AI 不经过命令事件）为空。
     */
    std::optional<std::uint32_t> mOperationCount;
    /** 合并前的计入类命令事件条数，仅供参考；与 mOperationCount 同时存在。 */
    std::optional<std::uint32_t> mOperationEventCount;
    /** 操作数 ÷ 游戏分钟数；非人类玩家或游戏时间过短时为空。 */
    std::optional<float> mApm;
    UpgradeStatus mBarracksUpgrade;
    UpgradeStatus mWarFactoryUpgrade;

    std::uint32_t GetBuiltTotal() const noexcept
    {
        return mBuiltBuildings + mBuiltVehicles + mBuiltInfantry + mBuiltAircraft;
    }
};

struct PlayerSummary
{
    int mHouseIndex = -1;
    std::string mName;
    std::string mFaction;
    std::uint32_t mColorRgb = 0xD4A753;
    bool mIsHuman = false;
    bool mDefeated = false;
    std::optional<std::int64_t> mCredits;
    std::optional<int> mPowerOutput;
    std::optional<int> mPowerDrain;
    bool mHasProductionData = false;
    bool mHasUnitData = false;
    std::vector<ProductionLine> mProductionLines;
    std::vector<UnitSummary> mUnits;
    /** 部队总数：步兵、载具、飞机，含矿车，不含建筑；缺失表示未读取。 */
    std::optional<std::uint32_t> mUnitTotal;
    /** 缺失表示统计层尚无该玩家的数据，不得当作零。 */
    std::optional<PlayerStatistics> mStatistics;
    std::optional<ProductionLineStatus> mBarracksLine;
    std::optional<ProductionLineStatus> mWarFactoryLine;
    /** 空指针表示统计层尚无该玩家的曲线；点数为零表示尚未采到样。 */
    std::shared_ptr<const EconomyCurve> mEconomy;
};

struct ControlGroup
{
    int mNumber = 0;
    std::vector<UnitSummary> mUnits;
    std::uint32_t mUnmatchedMemberCount = 0;
};

struct FpsSample
{
    std::uint32_t mFrameNumber = 0;
    float mFps = 0.0f;
};

/** 游戏画布中的矩形，单位为游戏内部像素，原点为 WindowBounds 左上角。 */
struct NativeRect
{
    int mX = 0;
    int mY = 0;
    int mWidth = 0;
    int mHeight = 0;
};

/**
 * 原生界面几何，由游戏线程采样，渲染层负责换算到客户区。
 * 只在侧栏位于右侧、非 Armageddon 且 SidebarBounds 完整落在画布内时发布；缺失时不得显示侧栏面板。
 */
struct NativeUiLayout
{
    int mCanvasWidth = 0;
    int mCanvasHeight = 0;
    /** DSurface::SidebarBounds：顶部资金条与雷达块（y < SidebarBounds.Y）以下的侧栏区，回放面板占用此区域。 */
    NativeRect mSidebarPanel;
    /** 原生底部命令栏；底栏据此加高以完整盖住它。缺失表示几何未知，底栏只按自身内容高度排版。 */
    std::optional<NativeRect> mCommandBar;
};

/** 世界坐标，单位与游戏 CoordStruct 相同（一格 256）；Z 已含地面高度与桥面抬升。 */
struct WorldPoint
{
    std::int32_t mX = 0;
    std::int32_t mY = 0;
    std::int32_t mZ = 0;
};

/**
 * 雷达事件里的一位玩家：名称与颜色在事件发生时复制，之后不再读取游戏。
 * mHouseIndex 为 -1 表示中立或没有所属房屋（例如无主建筑）。
 */
struct RadarPlayerRef
{
    int mHouseIndex = -1;
    std::string mName;
    std::uint32_t mColorRgb = 0x8A8F86;
};

/** 雷达事件提示栏目前只有两类：超武被释放、建筑被间谍渗透。 */
enum class RadarEventKind { SuperWeaponLaunched, BuildingInfiltrated };

/**
 * 一条雷达事件，发生后不可修改。时间为回放的游戏时间（毫秒），取事件所在帧完成时的游戏时钟。
 * 超武：mActor 是释放方，mSubjectName 是超武名称，mIcon 是超武图标（缺失时为空，界面画通用标记）。
 * 渗透：mActor 是间谍所属方，mVictim 是被渗透建筑的所属方（可为中立），mSubjectName 是建筑名称。
 */
struct RadarEvent
{
    /** 回放会话内单调递增、从 1 开始；界面用它作行身份，点击跳转命令也用它。 */
    std::uint64_t mId = 0;
    RadarEventKind mKind = RadarEventKind::SuperWeaponLaunched;
    std::int64_t mGameTimeMs = 0;
    RadarPlayerRef mActor;
    RadarPlayerRef mVictim;
    std::string mSubjectName;
    std::shared_ptr<const IconImage> mIcon;
    /** 事件位置（超武落点或被渗透建筑）；缺失时界面不提供“点击跳转”。 */
    std::optional<WorldPoint> mLocation;
};

/** 不可变的事件历史，按发生顺序排列（最新的在末尾），至多 RADAR_EVENT_HISTORY_LIMIT 条；快照之间共享。 */
struct RadarEventList
{
    std::vector<RadarEvent> mEvents;
};

enum class PlaybackStatus { Playing, Paused, Ended };

/**
 * 快进（向前跳转）的观看状态，由游戏线程写入快照。帧号与 mFrameNumber 同一单位（最后完成的帧号）：
 * 快进在第 mTargetFrame 帧完成后停下。默认值表示不在快进中。
 */
struct PlaybackSeekView
{
    bool mIsSeeking = false;
    /** 开始快进时的最后完成帧；只在 mIsSeeking 时有效。 */
    std::uint32_t mStartFrame = 0;
    /** 目标帧，大于 mStartFrame；只在 mIsSeeking 时有效。 */
    std::uint32_t mTargetFrame = 0;
};

/**
 * 快进进度（0 到 1）：(最后完成帧 − 起点) ÷ (目标 − 起点)，夹在 [0, 1]；目标不大于起点时为 1。
 * 游戏线程的快进逻辑与界面共用这一算法。
 */
constexpr float GetSeekProgress(std::uint32_t startFrame, std::uint32_t targetFrame,
    std::uint32_t lastCompletedFrame) noexcept
{
    if (targetFrame <= startFrame || lastCompletedFrame >= targetFrame)
        return 1.0f;
    if (lastCompletedFrame <= startFrame)
        return 0.0f;
    return static_cast<float>(static_cast<double>(lastCompletedFrame - startFrame) /
        static_cast<double>(targetFrame - startFrame));
}

struct PlaybackControls
{
    PlaybackStatus mStatus = PlaybackStatus::Playing;
    std::uint32_t mLastCompletedFrame = 0;
    bool mCanRestart = false;
    bool mIsHalfShroudEnabled = false;
    std::string mRestartError;
    /**
     * 可以向前跳转：快照激活、总帧数已知、回放未结束且未请求退出。快进中仍为真（可以改到更靠后的目标）。
     * 为假时界面只显示只读进度。
     */
    bool mCanSeek = false;
    PlaybackSeekView mSeek;
};

/**
 * 观看者选择的 HUD 缩放档位，作用于整个 HUD。Auto 按客户区高度选择实际档位（规则见 ui::ReplayHudScale）。
 * 枚举值就是设置文件与观看命令中使用的百分比，Auto 为 0；不得改变已有数值。
 */
enum class HudScaleSetting : int
{
    Auto = 0,
    Percent100 = 100,
    Percent125 = 125,
    Percent150 = 150,
    Percent175 = 175,
    Percent200 = 200
};

/** 把命令或设置中的百分比还原为档位；不是已定义档位时返回空。 */
constexpr std::optional<HudScaleSetting> ToHudScaleSetting(int value) noexcept
{
    switch (value)
    {
    case 0: return HudScaleSetting::Auto;
    case 100: return HudScaleSetting::Percent100;
    case 125: return HudScaleSetting::Percent125;
    case 150: return HudScaleSetting::Percent150;
    case 175: return HudScaleSetting::Percent175;
    case 200: return HudScaleSetting::Percent200;
    default: return std::nullopt;
    }
}

/**
 * 战局缩放的观看状态，由游戏线程按缩放适配器的状态换算后写入快照。
 * 默认值表示“不可用且没有原因”：录制、回放尚未开始或缩放未启用，界面不显示缩放控件。
 */
struct BattlefieldZoomView
{
    bool mIsAvailable = false;
    /** 已应用的倍数不是 1×。 */
    bool mIsActive = false;
    /** 已应用倍数的百分比（物理 ÷ 逻辑，四舍五入）：400、300、250、200、150、125、100、80、67、50、40、33。 */
    int mPercent = 100;
    /** 请求档位还能向放大或缩小方向移动；范围由物理战术区与内存预算决定，到头时为假。 */
    bool mCanZoomIn = false;
    bool mCanZoomOut = false;
    /** 观看者请求的档位尚未应用，例如拖框或镜头平移动画进行中被推迟。 */
    bool mHasPendingChange = false;
    /** 不可用时给界面的简短原因（简体中文）；不可用且为空时界面不显示缩放控件。 */
    std::string mUnavailableReason;
};

/** 游戏线程发布的只读值快照；缺失数据用 optional/能力标志表示，不能当作零。 */
struct ReplayViewSnapshot
{
    std::uint64_t mSessionId = 0;
    bool mActive = false;
    bool mIsPreview = false;
    std::string mReplayName;
    std::string mMapName;
    std::string mMapHash;
    std::optional<std::int64_t> mStartedUtc;
    bool mWasNetworkGame = false;
    std::uint32_t mFrameNumber = 0;
    /** 回放播放到当前帧的游戏时间；统计层尚未采到任何帧时为空。 */
    std::optional<GameTime> mGameTime;
    std::optional<std::uint32_t> mTotalFrames;
    /** 帧号 → 游戏时间表；空指针表示尚未生成（总帧数未知、扫描未结束或还缺运行时速度）。存在时元素数等于 mTotalFrames。 */
    std::shared_ptr<const GameTimeline> mTimeline;
    int mRecordedLocalHouseIndex = -1;
    int mPlaybackSpeedSetting = 0;
    bool mCanChangePlaybackSpeed = false;
    PlaybackControls mPlaybackControls;
    bool mHasPlayerVision = false;
    bool mHasRecordedGroups = false;
    std::optional<NativeUiLayout> mNativeUiLayout;
    /** 原生雷达是否可用；雷达跟随录制方本地玩家，与观看者所选玩家无关。缺失表示未采样。 */
    std::optional<bool> mIsNativeRadarAvailable;
    /** 游戏线程持有并持久化的权威设置；渲染线程据此重建字体并排版。没有设置时为 100%。 */
    HudScaleSetting mHudScaleSetting = HudScaleSetting::Percent100;
    std::vector<PlayerSummary> mPlayers;
    std::vector<ControlGroup> mRecordedGroups;
    std::vector<FpsSample> mRecordedFps;
    /** 雷达事件历史；空指针表示事件功能未启用，点数为零表示尚无事件。 */
    std::shared_ptr<const RadarEventList> mRadarEvents;
    BattlefieldZoomView mBattlefieldZoom;
};

/**
 * SetHudScale 的 mValue 为 HudScaleSetting 的数值（0 表示自动）。
 * CenterOnRadarEvent 的 mValue 为 RadarEvent::mId：游戏线程只移动本地镜头（与雷达点击相同的 SetTacticalPosition），
 * 事件已不在历史中或没有位置时忽略。
 * 战局缩放命令都在下一次绘制时由游戏线程应用，命令本身不带坐标：
 * - AdjustBattlefieldZoom（Ctrl+滚轮）的 mValue 为档数（正数放大，每格一档），以应用时鼠标指着的地面点为锚点，
 *   鼠标不在战术区内时退回战术区中心；
 * - AdjustBattlefieldZoomAtCenter（HUD 的－/＋按钮）的 mValue 同为档数，始终以战术区中心为锚点；
 * - ResetBattlefieldZoom 复原到 1×（以战术区中心为锚点），不看 mValue。
 * 向前跳转：
 * - SeekToFrame 的 mValue 为目标帧 L，与 mFrameNumber 同一单位：快进到第 L 帧完成后停下。游戏线程只接受大于当前
 *   最后完成帧的目标，超过最后一帧的按最后一帧处理；快进中只接受比当前目标更靠后的目标，并直接改目标；
 * - CancelSeek 结束正在进行的快进，停在当前位置，不看 mValue。
 * 界面在松开进度条时才提交 SeekToFrame，每次拖动只提交一条（命令队列容量有限，满了会丢掉最旧的）。
 * 新命令只能追加在末尾，不得改变已有枚举值。
 */
enum class ViewerCommandKind
{
    SetPlaybackSpeed, SetPaused, ExitPlayback, RestartPlayback, SetHalfShroud, SetHudScale, CenterOnRadarEvent,
    AdjustBattlefieldZoom, ResetBattlefieldZoom, AdjustBattlefieldZoomAtCenter, SeekToFrame, CancelSeek
};
struct ViewerCommand
{
    std::uint64_t mSessionId = 0;
    ViewerCommandKind mKind = ViewerCommandKind::SetPlaybackSpeed;
    int mValue = 0;
};
}
