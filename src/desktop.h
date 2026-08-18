#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

// 桌面集成：SysListView32 宿主查找、网格度量、快捷方式还原（参考 openFences）。

namespace desktop {

// 设置进程 Per-Monitor V2 感知（必须在任何窗口创建前调用）
void EnableDpiAwareness();

// 当前桌面图标列表窗口（SysListView32）。按需求 4.1 的查找顺序。
// 找不到时向 Progman 发送 0x052C 触发桌面层级建立后重试。
HWND FindDesktopListView();

// 读一次桌面图标网格单元格尺寸（LVM_GETITEMSPACING），缓存。
// 返回 false 表示暂不可用。
bool GetCellSize(WORD& cellW, WORD& cellH);

// 系统实际图标尺寸（物理像素，Per-Monitor V2 下 GetSystemMetrics 已含缩放）。
// 图标应以此为原生大小绘制，避免被放大失真。
int IconSize();

// 把屏幕坐标点换算成某窗口的客户区坐标
POINT ScreenToClientOf(HWND hwnd, POINT pt);

// 桌面文件夹路径
std::wstring DesktopFolder();

// 在桌面生成一个指向 target 的快捷方式 name.lnk（还原快捷方式用）
bool CreateDesktopShortcut(const std::wstring& name, const std::wstring& target);

// 判断路径是否为 .lnk
bool IsLnk(const std::wstring& path);
std::wstring LnkTarget(const std::wstring& lnkPath); // 解析 .lnk 指向的目标，失败返回空

} // namespace desktop
