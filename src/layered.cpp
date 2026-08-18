#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "layered.h"
#include <objidl.h>
#include <gdiplus.h>
#include <cstring>
#pragma comment(lib, "gdiplus.lib")

namespace {
ULONG_PTR g_gdiToken = 0;
}

namespace layered {

bool GlobalInit() {
    Gdiplus::GdiplusStartupInput in;
    return Gdiplus::GdiplusStartup(&g_gdiToken, &in, nullptr) == Gdiplus::Ok;
}

void GlobalShutdown() {
    if (g_gdiToken) Gdiplus::GdiplusShutdown(g_gdiToken);
    g_gdiToken = 0;
}

void Present(HWND hwnd, int w, int h, int screenX, int screenY,
             const std::function<void(Gdiplus::Graphics&, int, int)>& draw,
             BYTE alpha) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib) { DeleteDC(mem); return; }
    HBITMAP old = (HBITMAP)SelectObject(mem, dib);
    // 将 DIB 的整块内存按纯 32bpp ARGB 交给 GDI+，GDI+ 才会真正写入 alpha。
    // （用 Graphics(HDC) 时，BI_RGB 表面会被当作无 alpha 的 32bppRGB，alpha 恒为 0，
    //   导致整个图层全透明——图标/背景全画不出来。）
    Gdiplus::Bitmap gdib(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)bits);
    // 先清零为全透明（Bitmap 初次包装未必已清零）
    {
        Gdiplus::BitmapData bd;
        gdib.LockBits(&Gdiplus::Rect(0, 0, w, h),
                      Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &bd);
        if (bd.Scan0) memset(bd.Scan0, 0, (size_t)w * h * 4);
        gdib.UnlockBits(&bd);
    }
    {
        Gdiplus::Graphics g(&gdib);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));   // 全透明
        if (draw) draw(g, w, h);
    }

    // GDI+ 输出为 straight alpha；UpdateLayeredWindow 需要 premultiplied alpha，
    // 同时应用整体透明度。
    if (bits) {
        size_t n = (size_t)w * h;
        BYTE* p = (BYTE*)bits;
        for (size_t i = 0; i < n; i++) {
            BYTE a = p[i * 4 + 3];
            BYTE want = (BYTE)((unsigned)a * alpha / 255);
            p[i * 4 + 0] = (BYTE)((unsigned)p[i * 4 + 0] * want / 255); // B
            p[i * 4 + 1] = (BYTE)((unsigned)p[i * 4 + 1] * want / 255); // G
            p[i * 4 + 2] = (BYTE)((unsigned)p[i * 4 + 2] * want / 255); // R
            p[i * 4 + 3] = want;
        }
    }

    POINT pt{ screenX, screenY };
    SIZE sz{ w, h };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, nullptr, &pt, &sz, mem, &src, 0, &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

} // namespace layered
