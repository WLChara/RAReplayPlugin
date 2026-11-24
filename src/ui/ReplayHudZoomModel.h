#pragma once

#include "../viewer/ReplayViewModel.h"

#include <string>

/**
 * 底栏战局缩放控件的显示内容（不含 ImGui 的纯逻辑），只由快照决定。
 * 控件只提交观看命令，倍数、范围与是否待应用都以游戏线程发布的快照为准。
 */
namespace rareplay::ui
{
enum class ZoomControlMode
{
    /** 不显示：录制、回放尚未开始或缩放未启用（快照的缩放信息不可用且没有原因）。 */
    Hidden,
    /** 一行灰色说明“缩放不可用：原因”，没有按钮。 */
    Unavailable,
    /** 当前百分比与 －、＋、复原 三个按钮。 */
    Controls
};

/** Controls 模式的悬停提示：滚轮以鼠标为锚点，－/＋按钮以战术区中心为锚点。 */
inline constexpr char ZOOM_CONTROL_TOOLTIP[] = "Ctrl+滚轮缩放，以鼠标指向处为中心\n－/＋按钮以画面中心缩放";

struct ZoomControlModel
{
    ZoomControlMode mMode = ZoomControlMode::Hidden;
    /** Controls：“缩放 50%”，有待应用的换档时附“…”；Unavailable：“缩放不可用：原因”。 */
    std::string mLabel;
    /** 底栏同一行放不下完整文字时的短文字：Controls 只有百分比（含等待标记），Unavailable 为“缩放不可用”。 */
    std::string mCompactLabel;
    /** 按钮是否可用；快照未激活或正在快进（canInteract 为假）时全部不可用。 */
    bool mCanZoomIn = false;
    bool mCanZoomOut = false;
    /** 已应用或待应用的倍数不是 1× 时可以复原；稳定在 1× 时不可用。 */
    bool mCanReset = false;
};

ZoomControlModel BuildZoomControlModel(const viewer::BattlefieldZoomView& view, bool canInteract);
}
