#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

namespace Gdiplus { class Bitmap; }

// 图标解析：从 exe / lnk / 文件夹 / ico / 图片 获取图标。
namespace iconlib {

// 返回一个 HICON（调用方负责 DestroyIcon）。pwPath 可为空 → 默认图标。
// size 为逻辑像素（内部按 DPI 换算为实际像素）。
HICON IconForPath(const std::wstring& path, int size);

// 应用默认图标（占位）
HICON DefaultIcon(int size);

// 返回 GDI+ 位图形式的图标（调用方负责 delete）。
// 优先从 shell 获取高分辨率 32bpp ARGB 位图，保留 alpha 抗锯齿边缘。
// 失败时回退到 IconForPath 并转换为 Bitmap。
Gdiplus::Bitmap* IconBitmapForPath(const std::wstring& path, int size);

// 判断快捷方式是否为 UWP 快捷方式（无目标路径且无图标位置）。
bool IsUwpShortcut(const std::wstring& path);

} // namespace iconlib
