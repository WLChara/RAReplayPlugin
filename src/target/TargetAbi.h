#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rareplay::target
{
// 已核实的 gamemd.exe x86 固定版本；仅底层适配器与 Hook 使用。
constexpr std::uintptr_t START_SESSION = 0x52D9A0;
constexpr std::uintptr_t RANDOM_SEED_DEFAULT_READ = 0x52FDF9;
constexpr std::uintptr_t RANDOM_SEED_GUARDED_READ = 0x52FE5C;
constexpr std::uintptr_t MAIN_LOOP = 0x55D360;
// MainLoop 内调用 NATIVE_DRAW_ON_TOP 的完整 5 字节 CALL（E8 89 6B F9 FF）；回放暂停与退出在此门控，续点见下面三个常量。
constexpr std::uintptr_t VIEWER_LOOP_GATE = 0x55D8F2;
constexpr std::uintptr_t VIEWER_LOOP_CONTINUE = 0x55D8F7;
constexpr std::uintptr_t VIEWER_HELD_LOOP = 0x55D830;
constexpr std::uintptr_t VIEWER_EXIT_LOOP = 0x55DEC8;
// MainLoop 返回后调用 NATIVE_GAME_OVER（IDB 名 ShowSpecialDialog，处理选项、投降与读档菜单）的完整 5 字节 CALL（E8 0F FA FF FF）；
// 回放先预取确认 EOF 再决定是否转调原函数，续点 GAME_OVER_CONTINUE。
constexpr std::uintptr_t GAME_OVER_CALL = 0x48CE9C;
constexpr std::uintptr_t GAME_OVER_CONTINUE = 0x48CEA1;
constexpr std::uintptr_t NATIVE_GAME_OVER = 0x48C8B0;
constexpr std::uintptr_t NATIVE_DRAW_ON_TOP = 0x4F4480;
// 原生 KeyboardProcess 将此槽当作四方向位，不能按 SDK 的 FileName 指针解释。
constexpr std::uintptr_t VIEWER_SCROLL_KEYS = 0xABCE14;
constexpr std::uintptr_t SHROUD_RESOURCE = 0x89E794;
// GetPixels 的专用 SHROUD 绘制返回点：6 字节 MOV EDX,[ViewBounds.Y]（8B 15 A4 6F 88 00），无相对位移；半透明黑幕在此替换 EAX 中的像素指针。
constexpr std::uintptr_t SHROUD_PIXELS_RETURN = 0x47F136;
constexpr std::uintptr_t BEFORE_SIMULATION = 0x55DC99;
constexpr std::uintptr_t EXECUTE_EVENTS = 0x64C380;
// handleAny 的单条事件分派点：7 字节 mov dl,[ecx+ecx*2+8B4204h]（8A 94 49 04 42 8B 00）。
// 正常执行与单机过期执行两条路径在此汇合；此时 ECX = 37 × DoList 环索引，
// 事件地址 = DO_LIST_EVENTS + EVENT_BYTES × 环索引（111 = 3 × 37）。
constexpr std::uintptr_t EVENT_DISPATCH = 0x64C63D;
constexpr std::uintptr_t DO_LIST_EVENTS = 0x8B4204;
constexpr std::uint32_t EVENT_BYTES = 111;
constexpr std::uint32_t EVENT_DISPATCH_INDEX_SCALE = 37;
constexpr std::uint32_t DO_LIST_CAPACITY = 16384;
static_assert(EVENT_BYTES == 3 * EVENT_DISPATCH_INDEX_SCALE);

/** 把 EVENT_DISPATCH 处的 ECX 还原为 DoList 环索引；不是 37 的整数倍或越过环容量时返回 false。 */
constexpr bool TryDecodeEventDispatchIndex(std::uint32_t ecx, std::uint32_t& outRingIndex) noexcept
{
    if (ecx % EVENT_DISPATCH_INDEX_SCALE != 0 || ecx / EVENT_DISPATCH_INDEX_SCALE >= DO_LIST_CAPACITY)
        return false;
    outRingIndex = ecx / EVENT_DISPATCH_INDEX_SCALE;
    return true;
}

constexpr std::uintptr_t FRAME_PARSER_CALL = 0x55DE40;
constexpr std::uintptr_t FRAME_PARSER_CONTINUE = 0x55DE45;
constexpr std::uintptr_t FRAME_COMPLETE = 0x55DE7E;
constexpr std::uintptr_t PREWORLD_WAIT_CALL = 0x684B3E;
constexpr std::uintptr_t PREWORLD_WAIT_CONTINUE = 0x684B43;
constexpr std::uintptr_t SESSION_END = 0x48CEAF;
constexpr std::uintptr_t SIDEBAR_COMMAND_ENTRY = 0x6AAD00;
constexpr std::uintptr_t SIDEBAR_COMMAND_BLOCK_RETURN = 0x6AAD20;
// SidebarClass::Draw（0x6A6C30）的合流点：6 字节 cmp byte ptr [edi+53A5h], bl（38 9F A5 53 00 00，
// 文件偏移 0x2A6CCC），入口只有 0x6A6CC5 与 0x6A6CC7，无相对位移。跳到 SIDEBAR_CHROME_SKIP_CONTINUE
// （Strip 绘制调用 0x6A6FDF 之后）即跳过背景、按钮、标签动画与图标条；能量条、雷达、提示文字与
// BlitSidebar 照常。两点 ESP 相同，续点所需 ebx=0、edi=this、[esp+34h]、[esp+0Ch] 在此之前已写好。
constexpr std::uintptr_t SIDEBAR_CHROME_DRAW = 0x6A6CCC;
constexpr std::uintptr_t SIDEBAR_CHROME_SKIP_CONTINUE = 0x6A6FE4;
// 在 SIDEBAR_CHROME_DRAW 处 [esp+34h] 是 Draw 复用的 forceRedraw 参数槽，已由 0x6A6CBF/0x6A6CC7 按
// 参数与 +0x53A6/+0x53A7 算好；Draw 末尾 0x6A70B7/0x6A70BD 无条件清零这两个标志，所以中途置标志
// 必须同时把此字节置 1，等同原生在 Draw 入口前已请求重画。
constexpr int SIDEBAR_DRAW_FORCE_REDRAW_STACK = 0x34;
// HouseClass::Fire_SW（0x4FAE50）内围绕 SuperClass::ClickFire 调用（0x4FAE97）的一对观察点。
// 两点都不含相对位移，返回 0 由 Syringe 重放被覆盖的原指令。
// C1：5 字节 mov eax,[LocalPlayer]（A1 4C 3D A8 00，文件偏移 0xFAE85），续点 0x4FAE8A。
// C2：9 字节 mov edx,[edi+28h] + mov al,[edx+0EEh]（8B 57 28 8A 82 EE 00 00 00，文件偏移 0xFAE9C），续点 0x4FAEA5。
// 两点 EBX = 开火的 HouseClass*，EDI = SuperClass*；C1 的 ESI = 前置依赖超武（可为 0）。
// 栈：两点 ESP 相同（ClickFire 为 retn 8），[ESP+0x14] = 超武索引，[ESP+0x18] = const CellStruct*。
constexpr std::uintptr_t SUPERWEAPON_FIRE_BEFORE_CLICK = 0x4FAE85;
constexpr std::uintptr_t SUPERWEAPON_FIRE_AFTER_CLICK = 0x4FAE9C;
constexpr int SUPERWEAPON_FIRE_STACK_CELL = 0x18;
// InfantryClass::UpdatePosition（0x519630）调用 BuildingClass::Infiltrate（0x4571E0，0x51A00B）之前：6 字节
// mov eax,[esi+21Ch]（8B 86 1C 02 00 00，文件偏移 0x11A002），续点 0x51A008。ESI = 间谍 InfantryClass*，
// EDI = 被渗透的 BuildingClass*。MO 中只有 Ares 判定为“间谍渗透敌方建筑”时才会到达，已排除工程师与 C4。
constexpr std::uintptr_t INFANTRY_INFILTRATE_BUILDING = 0x51A002;
constexpr std::uintptr_t VIEWER_SPEED_GETTER = 0x4E209E;
constexpr std::uintptr_t VIEWER_SPEED_GETTER_CONTINUE = 0x4E20A4;
constexpr std::uintptr_t VIEWER_SPEED_SETTER = 0x4E1E1B;
constexpr std::uintptr_t VIEWER_SPEED_SETTER_CONTINUE = 0x4E1EC0;
constexpr std::uintptr_t VIEWER_FRAME_PACING = 0x55D79E;
constexpr std::uintptr_t VIEWER_FRAME_PACING_CONTINUE = 0x55D7A4;
constexpr std::uintptr_t VIEWER_MOUSE_INPUT_BEGIN = 0x4F4320;
constexpr std::uintptr_t VIEWER_MOUSE_INPUT_END = 0x4F43DC;
constexpr std::uintptr_t VIEWER_KEYBOARD_INPUT_BEGIN = 0x55DEE0;
// 与 VIEWER_KEYBOARD_INPUT_BEGIN 成对：键盘输入函数的全部正常出口，都在弹出栈帧之前，观众输入范围在此收尾。
// 出口有两种形式（已核对磁盘原字节）：6 字节 5E 5D 5B 83 C4 08（pop esi、pop ebp、pop ebx、add esp,8，后接 retn）；
// 7 字节 5E 5D A3 14 CE AB 00（pop esi、pop ebp、mov [VIEWER_SCROLL_KEYS],eax，后接 pop ebx、add esp,8、retn）。
constexpr std::array<std::uintptr_t, 11> VIEWER_KEYBOARD_INPUT_ENDS = {
    0x55E072, // 6 字节形式出口
    0x55E083, // 6 字节形式出口
    0x55E099, // 6 字节形式出口
    0x55E0B5, // 7 字节形式出口（写回方向键槽）
    0x55E0C4, // 7 字节形式出口（写回方向键槽）
    0x55E0E5, // 7 字节形式出口（写回方向键槽）
    0x55E0F4, // 7 字节形式出口（写回方向键槽）
    0x55E114, // 7 字节形式出口（写回方向键槽）
    0x55E122, // 7 字节形式出口（写回方向键槽）
    0x55E142, // 7 字节形式出口（写回方向键槽）
    0x55E155, // 6 字节形式出口（函数末尾）
};
constexpr std::uintptr_t VIEWER_CONTROLLED_BY_PLAYER = 0x50B6F0;
constexpr std::uintptr_t VIEWER_CONTROLLED_FALSE_RETURN = 0x50B70D;
constexpr std::uintptr_t ANIMATION_CONSTRUCTOR = 0x421EA0;
// 返回地址而非 Hook 点：本地移动点击在 0x4D7F93 调用 ANIMATION_CONSTRUCTOR 创建 MoveFlash 后返回到此；
// ANIMATION_CONSTRUCTOR 处的 Hook 用它与栈顶比较，只记录这一处构造。
constexpr std::uintptr_t MOVE_FLASH_ANIMATION_RETURN = 0x4D7F98;
// ObjectClass::SetHeight（0x5F5FA0，thiscall，retn 4）内的 IsOnMap 测试：5 字节 mov al,[esi+74h]; test al,al
// （8A 46 74 84 C0），无相对位移；其后 0x5F5FC0 的 jz SET_HEIGHT_Z_ONLY 不被覆盖，返回 0 时 Syringe 重放这两条指令、
// 由 test 重新设置标志位。此处 ESI = this（ObjectClass*），EDI = 已加桥高的高度；栈上 [ESP+0] 为保存的 EDI、
// [ESP+4] 为保存的 ESI、[ESP+8..0x13] 为局部坐标、[ESP+0x14] 为调用者返回地址、[ESP+0x18] 为高度参数。
constexpr std::uintptr_t SET_HEIGHT_MARK_TEST = 0x5F5FBB;
// SetHeight 原生“IsOnMap 为假”分支：只写 Location.Z = EDI + GetCellFloorHeight(Location)，不调 Mark（不改格内链表、
// 不调用 DiscoveredBy）。栈与寄存器约定同 Mark 分支，正常 retn 4；从 SET_HEIGHT_MARK_TEST 跳到此处即复用原生 Z 计算。
constexpr std::uintptr_t SET_HEIGHT_Z_ONLY = 0x5F6017;
// SET_HEIGHT_MARK_TEST 处调用者返回地址相对 ESP 的偏移（见上）。
constexpr int SET_HEIGHT_RETURN_ADDRESS_STACK = 0x14;
// 返回地址而非 Hook 点：AircraftClass::Draw（0x4144B0）画体素影子时调用 SetHeight（均为 6 字节 call [reg+1CCh]）后返回到此。
// SetHeight 也被模拟大量调用，而这三个返回地址只出现在这三处调用之后，所以在 SET_HEIGHT_MARK_TEST 处按栈上的
// 返回地址即可只识别影子绘制，不必核对对象类型，也不影响其他调用者。
constexpr std::array<std::uintptr_t, 3> AIRCRAFT_SHADOW_SET_HEIGHT_RETURNS = {
    0x4147DB, // 0x4147D5 处 SetHeight(桥高)：目标格及其北邻或西邻都是桥格等情形
    0x4147F9, // 0x4147F3 处 SetHeight(0)：其余全部情形
    0x4148B1, // 0x4148AB 处 SetHeight(原高度)：无条件恢复
};

/** 返回地址是否属于 AircraftClass::Draw 画影子时的三次 SetHeight 调用之一。 */
constexpr bool IsAircraftShadowSetHeightReturn(std::uintptr_t returnAddress) noexcept
{
    for (const std::uintptr_t candidate : AIRCRAFT_SHADOW_SET_HEIGHT_RETURNS)
    {
        if (candidate == returnAddress)
            return true;
    }
    return false;
}

// 原生赋组函数写入编队之后的出口：5 字节 pop edi、pop ebp、pop ebx、retn、nop（5F 5D 5B C3 90）；编队录制在此得知编队变化。
constexpr std::uintptr_t CONTROL_GROUP_ASSIGN_EXIT = 0x7310CB;
// 原生清组函数写入编队之后的出口，字节与 CONTROL_GROUP_ASSIGN_EXIT 相同；编队录制在此得知编队变化。
constexpr std::uintptr_t CONTROL_GROUP_CLEAR_EXIT = 0x73112B;
constexpr std::uintptr_t NATIVE_FRAME_PARSER = 0x647260;
constexpr std::uintptr_t NATIVE_PREWORLD_WAIT = 0x684370;
constexpr std::uintptr_t POST_FRAME_EVENTS = 0x64D990;
constexpr std::uintptr_t EXECUTOR_CONTEXT = 0xA8B548;
constexpr std::uintptr_t EXIT_REQUEST = 0xA83D48;
constexpr std::uintptr_t OPTIONS_REQUEST = 0xA8EDA0;
constexpr std::uintptr_t WORLD_READY = 0xA8ED5C;

// ---- 回放向前跳转 ----
// 帧预算：MainLoop 每帧开始时按速度重算，RegulateFrameRate（0x55E160，在 FRAME_COMPLETE 之后的 0x55DE9A 调用）读取；
// 在 FRAME_COMPLETE 写 0 只让本帧不等待，下一帧原生重算，不必恢复。
// int，单机预算，单位 SystemTimer tick（timeGetTime()>>4）；0x55D7BC 写入，为 0 时单机路径不 Sleep 直接到 FPS 采样。
constexpr std::uintptr_t FRAME_BUDGET = 0x887350;
// int，联机等待预算，单位 ms（1000/DesiredFrameRate）；为 0 时联机路径在 0x55E1CD–0x55E1ED 直接跳到 0x55E33B，没有额外帧与 Sleep。
constexpr std::uintptr_t FRAME_WAIT_BUDGET = 0x887330;
// 主音量斜坡指针全局（音效、语音、音乐），音频初始化 0x406B10 写入，未初始化时为空。
constexpr std::uintptr_t SOUND_VOLUME_RAMP = 0x87E748;
constexpr std::uintptr_t VOICE_VOLUME_RAMP = 0x87E740;
constexpr std::uintptr_t SCORE_VOLUME_RAMP = 0x87E744;
// AudioRamp_SetTargetImmediate：void __fastcall(ramp* ECX, int value EDX)，retn；函数体 ramp[0] |= 1、ramp[2] = value << 16，
// 混音线程见到 bit0 即把当前值跳到目标。原生选项 setter 同样不加锁写入。
constexpr std::uintptr_t AUDIO_RAMP_SET_TARGET_IMMEDIATE = 0x407150;
// VoxClass::StopCurrent：void __fastcall(bool clearQueue CL)，retn；停掉当前 EVA 并按参数清空队列，原生 Logic 也调用（0x53B508）。
constexpr std::uintptr_t VOX_STOP_CURRENT = 0x7529A0;
// 原生音量 setter 把 GameOptions 的 0..1 音量乘以它（flt_7EF38C = 16384.0）再经 F2I 写入斜坡。
constexpr double AUDIO_RAMP_VOLUME_SCALE = 16384.0;
// GScreenClass::MarkNeedsRedraw：void __thiscall(GScreenClass* = &MouseClass::Instance, int mode)，retn 4。置 Tactical 的
// Redrawing；mode 非 0 时写 MouseClass+0xC（已为 2 不覆盖），DrawOnTop 读出后清零：非 0 时三个 Render 阶段全重绘，等于 2 时
// 另有 TabClass::Draw(1)。YRpp 在 MapClass.h 以 RedrawSidebar(int) 声明同一地址（JMP_THIS），但内联汇编在使用处产生 C4731，
// 所以与 NATIVE_DRAW_ON_TOP 一样经地址调用。
constexpr std::uintptr_t GSCREEN_MARK_NEEDS_REDRAW = 0x4F42F0;
constexpr int FULL_REDRAW_WITH_SIDEBAR = 2;

// ---- 回放最快跳转：快进中的输入降频与积压清理 ----
// GScreenClass::GetInputAndUpdate（VIEWER_MOUSE_INPUT_BEGIN，thiscall，栈参数依次为 outKeyCode、outMouseX、outMouseY）的末条指令：
// 3 字节 retn 0Ch（C2 0C 00）。入口 Hook 处 ESP 指向返回地址，直接跳到这里即按原栈返回调用者并清掉三个参数；入口被覆盖的
// sub esp,8; push ebp; push esi（83 EC 08 55 56）不执行，也不经过 VIEWER_MOUSE_INPUT_END。MainLoop（0x55D8B4）与
// RegulateFrameRate（0x55E25C）随后只把键码交给 KeyboardProcess，键码为 0 时它在 0x55DEF2 直接返回，两个鼠标输出不被读取。
constexpr std::uintptr_t VIEWER_MOUSE_INPUT_SKIP_RETURN = 0x4F43E2;
// VIEWER_MOUSE_INPUT_BEGIN 处第一个栈参数（outKeyCode，DWORD*）相对 ESP 的偏移。
constexpr int VIEWER_MOUSE_INPUT_KEY_CODE_STACK = 0x4;

// 原生 DynamicVectorClass 的布局：+0 虚表、+4 Items、+8 Capacity、+0xC IsInitialized、+0xD IsAllocated、+0x10 Count、
// +0x14 CapacityIncrement（0x4C29E0、0x556A20、0x6D2790 按这些偏移读写）。以下几个全局都是这种向量，只在绘制路径中消费。
constexpr std::uintptr_t DYNAMIC_VECTOR_ITEMS_OFFSET = 0x4;
constexpr std::uintptr_t DYNAMIC_VECTOR_COUNT_OFFSET = 0x10;
// 战术区脏矩形 DynamicVectorClass<DirtyAreaStruct>（每项 20 字节）：TacticalClass::RegisterDirtyArea（0x6D2790）每次登记都与全表
// 比较合并，只有 TacticalClass::Render（0x6D3D10）消费。
constexpr std::uintptr_t DIRTY_AREAS = 0xB0CE78;
// DynamicVectorClass<EBolt*>：每发电击武器一个，只在 Render(2) 的 0x4C2830 按绘制次数过期。
constexpr std::uintptr_t EBOLT_ARRAY = 0x8A0E88;
// DynamicVectorClass<RadBeam*>：辐射与地裂光束，只在 Render 的 0x6591B0 按绘制次数过期；没有原生的整体清理函数。
constexpr std::uintptr_t RADBEAM_ARRAY = 0xB04A60;
// DynamicVectorClass<LineTrail*>：物体尾迹，拥有者解绑后只在 Render 的 0x556D40 中淡出并删除。
constexpr std::uintptr_t LINE_TRAIL_ARRAY = 0xABCB78;
// 雷达事件向量（DynamicVectorClass<RadarEventClass*> 布局），ClearRadarEvents 逐个删除。
constexpr std::uintptr_t RADAR_EVENT_ARRAY = 0xB04DA8;
static_assert(DIRTY_AREAS + DYNAMIC_VECTOR_COUNT_OFFSET == 0xB0CE88);
static_assert(EBOLT_ARRAY + DYNAMIC_VECTOR_COUNT_OFFSET == 0x8A0E98);
static_assert(RADBEAM_ARRAY + DYNAMIC_VECTOR_ITEMS_OFFSET == 0xB04A64);
static_assert(RADBEAM_ARRAY + DYNAMIC_VECTOR_COUNT_OFFSET == 0xB04A70);
static_assert(LINE_TRAIL_ARRAY + DYNAMIC_VECTOR_ITEMS_OFFSET == 0xABCB7C);
static_assert(LINE_TRAIL_ARRAY + DYNAMIC_VECTOR_COUNT_OFFSET == 0xABCB88);
static_assert(RADAR_EVENT_ARRAY + DYNAMIC_VECTOR_COUNT_OFFSET == 0xB04DB8);
// EBolt_DeleteAll：void __cdecl()，retn。逐个清除拥有者 UnitClass+0x6DC 的反向指针并用游戏 operator delete 释放，
// 然后 Count=0 并释放 Items（Capacity=0、IsAllocated=0；之后新电弧照常按 CapacityIncrement 扩容）。
constexpr std::uintptr_t EBOLT_DELETE_ALL = 0x4C29E0;
// ClearRadarEvents：void __cdecl()，retn（入口 push ecx 只作局部槽，不读 ECX）。从尾到头逐个移出并删除全部雷达事件。
constexpr std::uintptr_t CLEAR_RADAR_EVENTS = 0x65FD50;
// 游戏的 operator delete：void __cdecl(void*)，调用方 add esp,4 清栈（0x4C2A0C、0x6593CD、0x556DC6）。
constexpr std::uintptr_t GAME_OPERATOR_DELETE = 0x7C8B3D;
// GScreenClass::ScreenShakeX/Y（MouseClass::Instance 0x87F7E8 的 +4 与 +8，int），只在 DoBlit（0x4F4780）中衰减。
constexpr std::uintptr_t SCREEN_SHAKE_X = 0x87F7EC;
constexpr std::uintptr_t SCREEN_SHAKE_Y = 0x87F7F0;
static_assert(SCREEN_SHAKE_Y == SCREEN_SHAKE_X + 4);

// ---- 回放战局缩放 ----
// 以下 Hook 的原字节、相对位移、分支目标、寄存器与栈都经 IDA 对照原生指令逐一核对。
// 被覆盖范围内都没有相对位移。只做寄存器代入的 Hook 返回下一条指令（*_CONTINUE），其余返回 0 由 Syringe 重放原指令。

// H1（DrawOnTop 0x4F4480 内）：5 字节 mov eax,[g_CompositeSurface]（A1 1C 73 88 00），只从 0x4F44C9 顺序到达
// （时空特效淡出阶段不到达）。ESI = MouseClass this，EBP = 进入前的 g_TempSurface；返回 0 时原指令读到已换入的逻辑面。
constexpr std::uintptr_t ZOOM_DRAW_ENTER = 0x4F44CB;
// H1 处 [ESP+8] 的低字节是 Render 三个阶段共用的全重绘标志（与 E 处 EDI、[ESP+0xC] 为同一槽）。
constexpr int DRAW_ON_TOP_REDRAW_FLAG_STACK = 0x8;
// E：6 字节 mov ecx,[g_CompositeSurface]（8B 0D 1C 73 88 00），只作为 Render(0) 的返回地址到达。EDI 的低字节是全重绘标志
// （高字节未初始化）；此时 g_TileSurfaceSlot 未加锁，LastTacticalPos 尚未更新。
constexpr std::uintptr_t ZOOM_TILE_EXPOSED = 0x4F44E4;
// H2：6 字节 mov eax,[esp+10h]; mov edx,[esi]（8B 44 24 10 8B 16），Render(1) 之后、TabClass::Draw 之前，只顺序到达。
constexpr std::uintptr_t ZOOM_TAB_DRAW_BEGIN = 0x4F44F9;
// H3：6 字节 mov ecx,[g_CompositeSurface]（8B 0D 1C 73 88 00），TabClass::Draw 返回后、Render(2) 之前。
constexpr std::uintptr_t ZOOM_TAB_DRAW_END = 0x4F4505;
// H4：Render（0x6D3D10）阶段 2 在 0x6D493E 解锁 pSurface 之后：6 字节 mov edx,[g_ScenarioClassInstance]（8B 15 30 B2 A8 00），
// 只从 0x6D493E 顺序到达。EBX = 本帧 pSurface（已解锁），EBP = TacticalClass*，ECX/EDX 可写。之后的计时器、中央文字与
// Ares 0x6D49D1/0x6D4B25/0x6D4E79 按物理值画进 Pframe，所以 H4 必须恢复物理状态。
constexpr std::uintptr_t ZOOM_WORLD_DRAWN = 0x6D4941;
// D：5 字节 mov al,[g_SidebarRedrawRequested]（A0 19 B5 B0 00），Render(2) 之后；也是 0x4F44C9（时空特效淡出）的分支目标，
// 两路 ESP 相同。窗口仍激活（H4 未到达）时做与 H4 相同的收尾。
constexpr std::uintptr_t ZOOM_DRAW_TAIL = 0x4F451B;

// R2：sub_6D2360（像素偏移转 lepton；BuildingClass::PlayAnim、UpdatePlacement 等模拟侧调用）以战术视口宽高为阈值，
// 缩放时代入物理 P，使模拟结果与缩放无关。
// R2a：6 字节 mov esi,[TACTICAL_VIEWPORT_WIDTH]（8B 35 30 CE B0 00），ECX = Tactical、EDX = 像素偏移指针；非分支目标。
constexpr std::uintptr_t ZOOM_SIM_VIEWPORT_WIDTH = 0x6D2368;
constexpr std::uintptr_t ZOOM_SIM_VIEWPORT_WIDTH_CONTINUE = 0x6D236E;
// R2b：5 字节 mov eax,[TACTICAL_VIEWPORT_HEIGHT]（A1 34 CE B0 00），EDX = 偏移 Y。
constexpr std::uintptr_t ZOOM_SIM_VIEWPORT_HEIGHT = 0x6D237B;
constexpr std::uintptr_t ZOOM_SIM_VIEWPORT_HEIGHT_CONTINUE = 0x6D2380;

// RV：RegisterCellAsVisible 0x6DA7D0（逻辑帧揭雾）的视口判定读 g_ViewBounds 宽高，缩放时代入战术视口宽高（逻辑视图）。
// RV-a：6 字节 mov edx,[g_ViewBounds.Width]（8B 15 A8 6F 88 00），ESI = relX，EAX = relY，ECX = VB.X，EBX = this，EDI = pCell。
constexpr std::uintptr_t ZOOM_VISIBLE_CELL_WIDTH = 0x6DA916;
constexpr std::uintptr_t ZOOM_VISIBLE_CELL_WIDTH_CONTINUE = 0x6DA91C;
// RV-b：6 字节 mov ecx,[g_ViewBounds.Height]（8B 0D AC 6F 88 00），EDX = VB.Y。
constexpr std::uintptr_t ZOOM_VISIBLE_CELL_HEIGHT = 0x6DA92A;
constexpr std::uintptr_t ZOOM_VISIBLE_CELL_HEIGHT_CONTINUE = 0x6DA930;

// CW/CH：镜头 clamp ClampTacticalViewportOrigin 0x6D8640（实为 stdcall，retn 4）每轴唯一的宽高读取；EBX 同时用于半宽与
// 整宽，代入 min(战术视口, 物理 P)。直线代码，非分支目标。
// CW：6 字节 mov ebx,[g_ViewBounds.Width]（8B 1D A8 6F 88 00），push ebx 已执行。
constexpr std::uintptr_t ZOOM_CAMERA_CLAMP_WIDTH = 0x6D864E;
constexpr std::uintptr_t ZOOM_CAMERA_CLAMP_WIDTH_CONTINUE = 0x6D8654;
// CH：6 字节 mov ebx,[g_ViewBounds.Height]（8B 1D AC 6F 88 00）。
constexpr std::uintptr_t ZOOM_CAMERA_CLAMP_HEIGHT = 0x6D868A;
constexpr std::uintptr_t ZOOM_CAMERA_CLAMP_HEIGHT_CONTINUE = 0x6D8690;

// RC1/RC2：雷达点击 sub_6539D0 对目标格按“半屏”预夹紧（之后才由 SetTacticalPosition 做像素 clamp），代入 min(战术视口, 物理 P)。
// 0x653A3F 的 VB.Width 读取是侧栏 X 偏移，保持物理，不挂。
// RC1：6 字节 mov ecx,[g_ViewBounds.Width]（8B 0D A8 6F 88 00），0x653D87/0x653D8C 跳到其起点；SI/DI = 目标格。
constexpr std::uintptr_t ZOOM_RADAR_CLICK_WIDTH = 0x653D92;
constexpr std::uintptr_t ZOOM_RADAR_CLICK_WIDTH_CONTINUE = 0x653D98;
// RC2：6 字节 mov ebx,[g_ViewBounds.Height]（8B 1D AC 6F 88 00），ECX 仍为宽度值，EDX/EAX 是之后还要用的中间商。
constexpr std::uintptr_t ZOOM_RADAR_CLICK_HEIGHT = 0x653DAC;
constexpr std::uintptr_t ZOOM_RADAR_CLICK_HEIGHT_CONTINUE = 0x653DB2;

// RF1/RF2：雷达视框 sub_656EC0（RadarClass::Draw 0x653374 调用，属 TabClass::Draw）在两点之间的直线代码里 6 次读 VB 宽高，
// 其中一次是 fild，无法换寄存器；入口把 VB 宽高临时换成战术视口宽高，出口恢复。
// RF1：5 字节 mov eax,[g_ViewBounds.Width]（A1 A8 6F 88 00），是 0x656F0B jz 的目标（落在起点）。ESI = MouseClass this。
constexpr std::uintptr_t ZOOM_RADAR_FRAME_BEGIN = 0x656F12;
// RF2：5 字节 fstp st; lea ecx,[eax+eax]（DD D8 8D 0C 00），EAX = 刚读出的 VB.H；此时 x87 栈有 1 项，Hook 函数不得改动 x87 栈。
constexpr std::uintptr_t ZOOM_RADAR_FRAME_END = 0x65707D;

// AU：位置音效 sub_750AC0 已把 VB 半宽、半高（float，由 fild/fmul 0.5f 求得）存入栈上，紧接着读半宽求全宽：
// 6 字节 fld [esp+20h]; fadd st,st（D9 44 24 20 DC C0），此时 x87 栈为空。改写两个栈槽为战术视口宽高 × 0.5f。
constexpr std::uintptr_t ZOOM_SOUND_FIELD = 0x750B00;
constexpr int SOUND_FIELD_HALF_WIDTH_STACK = 0x20;
constexpr int SOUND_FIELD_HALF_HEIGHT_STACK = 0x24;

// 三个战场输入入口，入参都是指向“战术区相对物理点”（Point2D）的指针，位于 [ESP+4]，调用方只读不写；
// 换成插件持有的逻辑坐标副本，调用方的局部变量与原生保存的按下点保持物理。
// PC：ProcessClickCoords 入口，7 字节 mov eax,[esp+10h]; sub esp,18h（8B 44 24 10 83 EC 18），ECX = MouseClass，retn 18h。
constexpr std::uintptr_t ZOOM_PROCESS_CLICK = 0x692300;
// BS：Tactical_BeginDragBand 入口，6 字节 mov eax,[ecx+0D90h]（8B 81 90 0D 00 00），ECX = Tactical，[ESP+4] = &DisplayClass+0x11D4。
constexpr std::uintptr_t ZOOM_DRAG_BAND_BEGIN = 0x6D9F80;
// BE：Tactical_UpdateDragBandEnd 入口，同样 6 字节，[ESP+4] = LeftMouseButtonDown 栈上钳位后的物理当前点。
constexpr std::uintptr_t ZOOM_DRAG_BAND_END = 0x6D9FC0;
constexpr int ZOOM_INPUT_POINT_STACK = 0x4;

// PL1：规划模式入口 ProcessPlanningModeButtonBranchA（热键按下与命令条按钮共用）：5 字节 mov eax,[0xA8B538]
// （A1 38 B5 A8 00，SessionClass+0x300 观察者标志），CL = actionState，函数内无跳入。回放观看期间置 EAX=1 并续到下一条指令，
// 借用原生观察者分支（push ebx; test eax,eax; jnz → pop ebx; retn），不调用唯一置位规划标志的 sub_6379C0。
constexpr std::uintptr_t PLANNING_MODE_ENTER = 0x731A50;
constexpr std::uintptr_t PLANNING_MODE_ENTER_CONTINUE = 0x731A55;
// PL2：BranchB 0x731A70 中 call sub_637A10 之后的同样 5 字节 A1 38 B5 A8 00；已 push ebx，BL = actionState。
// EAX=1 续到 test eax,eax; jnz 0x731A8A，跳过 sub_63A120 的 Intro3 提示。
constexpr std::uintptr_t PLANNING_MODE_EXIT_NOTICE = 0x731A78;
constexpr std::uintptr_t PLANNING_MODE_EXIT_NOTICE_CONTINUE = 0x731A7D;

// 战术视口 0xB0CE28..0xB0CE34（{X,Y,W,H}）的宽与高；运行期唯一写者 Tactical_SetViewportRectAndRecenter。缩放激活期间常驻 L。
// YRpp 只在 TacticalClass::CoordsToClient 内以局部引用声明。
constexpr std::uintptr_t TACTICAL_VIEWPORT_WIDTH = 0xB0CE30;
constexpr std::uintptr_t TACTICAL_VIEWPORT_HEIGHT = 0xB0CE34;
// byte：Render 阶段间的全重绘标志，只由 Render 写（0x6D3DB4、0x6D3E3F）；Render(1) 在 0x6D3E4B 读它强制全重绘。只读。
constexpr std::uintptr_t TACTICAL_FULL_REDRAW_REQUESTED = 0xB0E63C;
// byte：ChronoScreenEffect 阶段 2 结束时嵌套调用 DrawOnTop 做离屏重绘期间为 1（0x53B664 置位，返回后清零）。只读。
constexpr std::uintptr_t CHRONO_OFFSCREEN_REDRAW = 0xA9FA94;
// TacticalClass::ZoomInFactor（+0xC0，double）为 1.0 的位模式；DoBlit 0x4F4928–0x4F4934 也按位比较。
constexpr std::uint64_t ZOOM_IN_FACTOR_IDENTITY_BITS = 0x3FF0000000000000ull;
// RegisterCellAsVisible 0x6DA93E cmp eax,31Fh / jge：VisibleCells 计数达到 799 后新格静默丢弃。
constexpr int VISIBLE_CELL_LIMIT = 799;

// VDSurface_Create：DSurface* __thiscall(void* storage, int width, int height, bool requestSystemMemory, bool request3DDevice)，
// retn 10h，返回 ECX。存储由调用者用游戏 operator new 分配；requestSystemMemory 由 cmp al,1 判定，须恰为 1。
// CreateSurface 失败仍返回对象，+0x1C（IDirectDrawSurface*）为空。销毁走虚表槽 0（0x4C1AC0，flags=1 时用游戏 delete 释放）。
constexpr std::uintptr_t VDSURFACE_CREATE = 0x4BA5A0;
constexpr std::size_t VDSURFACE_OBJECT_BYTES = 0x24;
// ZBuffer/ABuffer 对象大小：原生 push 30h; call operator new（0x4A8A02、0x4A8A9F）。
constexpr std::size_t DEPTH_BUFFER_OBJECT_BYTES = 0x30;
// GetRGBMode：A1 D0 05 82 00 C3（mov eax,[0x8205D0]; ret），无参数、不清栈，任何调用约定等价。
constexpr std::uintptr_t GET_RGB_MODE = 0x4BBC90;
constexpr int RGB_MODE_555 = 0;
constexpr int RGB_MODE_565 = 2;
// Tactical_SetViewportRectAndRecenter（原 sub_6D5F60）：void __thiscall(TacticalClass*, const RectangleStruct*)，retn 4。
// 写视口 → 以目标中心 clamp → 当前中心 = 目标中心 → UpdateVisibleAreaProjection → Redrawing=1；无 RNG。
constexpr std::uintptr_t TACTICAL_SET_VIEWPORT_RECT = 0x6D5F60;
// 以等距像素中心设镜头（IDB 误名 ApplyReplayFrameSyncHeader）：void __thiscall(TacticalClass*, const Point2D*)，retn 4。
// clamp → 写当前与目标中心 → UpdateVisibleAreaProjection → Redrawing=1；无 RNG，不写 Last* 字段。
constexpr std::uintptr_t TACTICAL_SET_CENTER_PIXEL = 0x6D6000;
}
