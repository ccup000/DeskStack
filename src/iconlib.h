#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

// 图标解析：从 exe / lnk / 文件夹 / ico / 图片 获取图标。
namespace iconlib {

// 返回一个 HICON（调用方负责 DestroyIcon）。pwPath 可为空 → 默认图标。
// size 为逻辑像素（内部按 DPI 换算为实际像素）。
HICON IconForPath(const std::wstring& path, int size);

// 应用默认图标（占位）
HICON DefaultIcon(int size);

} // namespace iconlib
