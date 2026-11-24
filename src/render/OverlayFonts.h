#pragma once

struct ImGuiIO;

namespace rareplay::render
{
/**
 * 清空字体图集，再按 18 像素 × scale 从 Windows 系统字体目录装载简体中文字体；失败时按同一比例安装
 * ImGui 默认字体。返回是否装载到中文字体。
 *
 * 只改 CPU 侧图集。若图集已上传到图形设备，调用方必须先销毁后端设备对象（OpenGL 的
 * DestroyDeviceObjects、D3D9 的 InvalidateDeviceObjects），之后由后端 NewFrame 按新图集重建纹理。
 * 只能在持有该 ImGui context 的渲染线程、ImGui::NewFrame 之前调用。
 */
bool LoadOverlayFont(ImGuiIO& io, float scale = 1.0f) noexcept;
}
