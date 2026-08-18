#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include <windows.h>
#include <functional>

namespace Gdiplus { class Graphics; }

// WS_EX_LAYERED 每像素透明渲染工具（32bpp DIB + GDI+ + UpdateLayeredWindow）。
namespace layered {

bool GlobalInit();                 // GdiplusStartup，main 调用一次
void GlobalShutdown();

// w,h 逻辑尺寸；draw 在 GDI+ 图形上绘制（0..w,0..h，透明背景）。
// 完成后做 alpha 预乘并 UpdateLayeredWindow 合入。
void Present(HWND hwnd, int w, int h, int screenX, int screenY,
             const std::function<void(Gdiplus::Graphics&, int, int)>& draw,
             BYTE alpha = 255);

} // namespace layered
