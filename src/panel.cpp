#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include "panel.h"
#include "container.h"
#include "iconlib.h"
#include "desktop.h"
#include "layered.h"
#include "appstate.h"
#include <objidl.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <cmath>
#include <algorithm>

using namespace Gdiplus;

namespace {

const WCHAR* kFont = L"Microsoft YaHei UI";

void FillRounded(Graphics& g, int x, int y, int w, int h, int r, const Color& c) {
    GraphicsPath gp;
    int d = r * 2;
    gp.AddArc(x, y, d, d, 180, 90);
    gp.AddArc(x + w - d, y, d, d, 270, 90);
    gp.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    gp.AddArc(x, y + h - d, d, d, 90, 90);
    gp.CloseFigure();
    SolidBrush br(c);
    g.FillPath(&br, &gp);
}

int EstimateTextWidth(const WCHAR* text, int fontSize) {
    if (!text) return 0;
    int w = 0;
    for (const WCHAR* p = text; *p; p++) {
        if (*p >= 0x4E00 && *p <= 0x9FFF) w += fontSize;       // 中日韩
        else w += (int)(fontSize * 0.55f);
    }
    return w;
}

// 文字换行相关常量（可能后续需要微调）
// 布局用的文字宽度：限制在合理范围内（过长用省略号），避免间距/半径被长名撑大
int LayoutLabelWidth(const WCHAR* t, int fontSize) {
    int w = EstimateTextWidth(t, fontSize);
    int cap = fontSize * g_settings.maxChars;   // 每行宽度上限约 6 字宽
    return w < cap ? w : cap;
}

// 将文本按最多 g_settings.maxChars 字符/行、最多 g_settings.maxLines 行进行换行。
// 有空格时优先按空格断行；超出 3 行时第 3 行末尾加省略号。
std::vector<std::wstring> WrapText(const std::wstring& text, int maxChars, int maxLines) {
    if (maxChars <= 0) maxChars = 1;
    if (maxLines <= 0) maxLines = 1;
    std::vector<std::wstring> result;
    if (text.empty()) return { L"" };
    if ((int)text.size() <= maxChars) return { text };

    size_t start = 0;
    while (start < text.size() && (int)result.size() < maxLines) {
        size_t end = std::min(start + maxChars, text.size());
        // 优先在空格处断行
        if (end < text.size()) {
            size_t sp = text.rfind(L' ', end);
            if (sp != std::wstring::npos && sp > start)
                end = sp;
        }

        if ((int)result.size() == maxLines - 1) {
            // 最后一行：若还有剩余，截断并加省略号
            if (end < text.size()) {
                size_t keep = start + (maxChars > 1 ? maxChars - 1 : 0);
                if (keep < start) keep = start;
                std::wstring last = text.substr(start, keep - start) + L"…";
                result.push_back(last);
            } else {
                result.push_back(text.substr(start));
            }
            start = text.size();
        } else {
            result.push_back(text.substr(start, end - start));
            if (end < text.size() && text[end] == L' ') end++;
            start = end;
        }
    }
    return result;
}

int WrappedLineCount(const std::wstring& text, int maxChars, int maxLines) {
    return (int)WrapText(text, maxChars, maxLines).size();
}

void DrawLabel(Graphics& g, const WCHAR* text, int cx, int cy, float size, int width, const Color& color) {
    if (!width) width = util::Scaled(120);
    Font fn(kFont, size, FontStyleRegular, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);   // 超宽省略号，避免溢出
    int lh = (int)(size + util::Scaled(6));
    RectF rc((float)(cx - width / 2), (float)(cy - lh / 2), (float)width, (float)lh);
    SolidBrush br(color);
    g.DrawString(text, -1, &fn, rc, &sf, &br);
}

void DrawWrappedLabel(Graphics& g, const WCHAR* text, int cx, int cy,
                      float size, int width, const Color& color) {
    std::vector<std::wstring> lines = WrapText(text ? text : L"",
                                               g_settings.maxChars, g_settings.maxLines);
    int lh = (int)(size + util::Scaled(6));
    int totalH = lh * (int)lines.size();
    int y0 = cy - totalH / 2;
    for (size_t i = 0; i < lines.size(); i++) {
        DrawLabel(g, lines[i].c_str(), cx, y0 + lh * (int)i + lh / 2,
                  size, width, color);
    }
}

void DrawHIcon(Graphics& g, HICON ic, int x, int y, int w, int h) {
    if (!ic) return;
    // FromHICON 会保留图标的 alpha/遮罩通道，透明区域不会出现白底残留
    Bitmap* bmp = Bitmap::FromHICON(ic);
    if (!bmp) return;
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.DrawImage(bmp, x, y, w, h);
    delete bmp;
}

void DrawBitmapIcon(Graphics& g, Bitmap* bmp, int x, int y, int w, int h) {
    if (!bmp) return;
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.DrawImage(bmp, x, y, w, h);
}

// 判断位图是否适合作为图标：仅排除全透明/空白位图。
// 注意：不能根据“黑色”判断占位图，因为自定义 .ico 也可能是黑色图标。
bool IsUsableIconBitmap(Bitmap* bmp) {
    if (!bmp) return false;
    int w = bmp->GetWidth(), h = bmp->GetHeight();
    if (w <= 0 || h <= 0) return false;
    int opaque = 0, checked = 0;
    for (int y = 0; y < h; y += 4) {
        for (int x = 0; x < w; x += 4) {
            Color c;
            if (bmp->GetPixel(x, y, &c) == Ok && c.GetA() > 0) {
                if (++opaque >= 4) return true;
            }
            if (++checked >= 256) break;
        }
        if (checked >= 256) break;
    }
    return opaque >= 4;
}

// plate（轴对齐矩形）四个角相对容器中心的最小/最大角（度）。
// 使用相对中心角的差值归一化到 [-180,180]，避免 0°/360° 跳变导致的范围计算错误。
void PlateAngularRange(double R, double thetaDeg, double halfW,
                       double topInset, double bottomOut,
                       double& minDeg, double& maxDeg) {
    const double PI = 3.141592653589793;
    double th = thetaDeg * PI / 180.0;
    double cx = R * std::cos(th);
    double cy = R * std::sin(th);
    double xs[4] = { cx - halfW, cx + halfW, cx - halfW, cx + halfW };
    double ys[4] = { cy - topInset, cy - topInset, cy + bottomOut, cy + bottomOut };
    minDeg = 360.0;
    maxDeg = -360.0;
    for (int i = 0; i < 4; i++) {
        double ang = std::atan2(ys[i], xs[i]) * 180.0 / PI;
        double rel = ang - thetaDeg;
        while (rel > 180.0) rel -= 360.0;
        while (rel < -180.0) rel += 360.0;
        double absDeg = thetaDeg + rel;
        if (absDeg < minDeg) minDeg = absDeg;
        if (absDeg > maxDeg) maxDeg = absDeg;
    }
}

// 为使首条 plate 不越过扇形起始边 startDeg，中心需要向内的最小角向偏移
double RequiredBoundaryInset(double R, double startDeg, double halfW,
                             double topInset, double bottomOut) {
    double lo = 0.0, hi = 90.0;
    for (int i = 0; i < 60; i++) {
        double mid = (lo + hi) / 2.0;
        double mn = 0.0, mx = 0.0;
        PlateAngularRange(R, startDeg + mid, halfW, topInset, bottomOut, mn, mx);
        if (mn >= startDeg - 1e-6) hi = mid;
        else lo = mid;
    }
    return hi;
}

// 由图标中心求出“实际区域”（图标+文字包围盒）
RECT PlateRect(const POINT& center, int iconSize, int labelW, int labelH,
               int vpad, int hpad) {
    int w = (labelW > iconSize ? labelW : iconSize) + 2 * hpad;
    int h = iconSize + util::Scaled(6) + labelH + 2 * vpad;
    // 区域比图标中心略偏下（因为下面还有文字）
    int top = center.y - iconSize / 2 - vpad;
    RECT rc = { center.x - w / 2, top, center.x + w / 2, top + h };
    return rc;
}

// Windows 存档夹式 hover 底色（半透明圆角矩形）
void FillPlate(Graphics& g, const RECT& rc, const Color& c) {
    int r = util::Scaled(6);
    FillRounded(g, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, r, c);
}

// 图标解析失败时绘制可见占位块（保证条目始终有图标可视）
void DrawFallbackIcon(Graphics& g, const WCHAR* text, int x, int y, int w, int h) {
    // 圆角底色 + 首字符
    GraphicsPath gp;
    int r = w / 4;
    gp.AddArc(x, y, r, r, 180, 90);
    gp.AddArc(x + w - r, y, r, r, 270, 90);
    gp.AddArc(x + w - r, y + h - r, r, r, 0, 90);
    gp.AddArc(x, y + h - r, r, r, 90, 90);
    gp.CloseFigure();
    SolidBrush br(Color(235, 0x66, 0x99, 0xEE));
    g.FillPath(&br, &gp);
    if (text && text[0]) {
        wchar_t ch[2] = { text[0], 0 };
        Font fn(kFont, (REAL)util::Scaled(28), FontStyleBold, UnitPixel);
        SolidBrush fg(Color(255, 255, 255, 255));
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        RectF rc((float)x, (float)y, (float)w, (float)h);
        g.DrawString(ch, -1, &fn, rc, &sf, &fg);
    }
}

// ── 拖动条目到桌面时的半透明拖影窗口 ─────────────────────────────
LRESULT CALLBACK DragImageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;   // 鼠标穿透，不抢焦点
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void DrawDragBitmap(Graphics& g, int w, int h, const ShortcutEntry& e, int iconSize) {
    // 拖影：圆角半透明底 + 图标 + 文字，跟随后续鼠标移动
    FillRounded(g, 0, 0, w, h, util::Scaled(8), Color(190, 255, 255, 255));
    int ix = (w - iconSize) / 2;
    int iy = util::Scaled(4);
    Bitmap* iconBmp = iconlib::IconBitmapForPath(e.path, iconSize);
    if (IsUsableIconBitmap(iconBmp)) {
        DrawBitmapIcon(g, iconBmp, ix, iy, iconSize, iconSize);
        delete iconBmp;
    } else {
        if (iconBmp) delete iconBmp;
        HICON ic = iconlib::IconForPath(e.path, iconSize);
        if (ic) {
            DrawHIcon(g, ic, ix, iy, iconSize, iconSize);
            DestroyIcon(ic);
        } else {
            DrawFallbackIcon(g, e.name.c_str(), ix, iy, iconSize, iconSize);
        }
    }
    int ly = iy + iconSize + util::Scaled(4) + util::Scaled(13) / 2;
    int lw = w - util::Scaled(4);
    if (lw < util::Scaled(20)) lw = util::Scaled(20);
    DrawLabel(g, e.name.c_str(), w / 2, ly, (REAL)util::Scaled(13), lw,
              Color(255, 0x20, 0x20, 0x20));
}

} // namespace

void PanelWindow::RegisterClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = PanelWindow::WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DeskStackPanel";
    RegisterClassExW(&wc);

    // 拖出条目时显示的拖影窗口（半透明、鼠标穿透）
    WNDCLASSEXW dwc = { sizeof(dwc) };
    dwc.lpfnWndProc = DragImageWndProc;
    dwc.hInstance = hInst;
    dwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dwc.lpszClassName = L"DeskStackDragImage";
    RegisterClassExW(&dwc);
}

PanelWindow::PanelWindow(ContainerWindow* owner, const ContainerData& data)
    : m_owner(owner), m_data(data) {
    std::vector<POINT> centers;
    // 触发容器屏幕中心（Fan/Ring 以此为圆心）
    RECT cr = {}; HWND ctx = m_owner ? m_owner->Hwnd() : nullptr;
    GetWindowRect(ctx, &cr);
    int cex = cr.left + (cr.right - cr.left) / 2;
    int cey = cr.top + (cr.bottom - cr.top) / 2;
    ComputeLayout(centers);
    int cw = m_windowW, ch = m_windowH;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST;
    m_hwnd = CreateWindowExW(ex, L"DeskStackPanel", L"",
                             WS_POPUP, m_windowX, m_windowY, cw, ch,
                             nullptr, nullptr, hInst, this);
    if (!m_hwnd) return;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);

    // 布局用的中心转换为客户端坐标
    m_centers.resize(centers.size());
    for (size_t i = 0; i < centers.size(); i++) {
        m_centers[i].x = centers[i].x - m_windowX;
        m_centers[i].y = centers[i].y - m_windowY;
    }
    // 面板中心（客户端）＝最终使用的屏幕圆心 − 面板左上
    // （环形可能因屏幕边界被平移，不能直接用触发容器中心）
    m_cx = m_screenCenterX - m_windowX;
    m_cy = m_screenCenterY - m_windowY;

    // 依据“实际区域”（图标+文字包围盒）构造每个条目的 plate
    m_plates.resize(m_centers.size());
    {
        int labelH = (int)(util::Scaled(13) + util::Scaled(6));
        int vpad = util::Scaled(6), hpad = util::Scaled(8);
        for (size_t i = 0; i < m_centers.size() && i < m_data.shortcuts.size(); i++) {
            int lw = LayoutLabelWidth(m_data.shortcuts[i].name.c_str(), util::Scaled(13));
            int lines = WrappedLineCount(m_data.shortcuts[i].name, g_settings.maxChars, g_settings.maxLines);
            m_plates[i] = PlateRect(m_centers[i], m_iconSize, lw,
                                    labelH * lines, vpad, hpad);
        }
    }
    Render();
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);   // 让面板可取得焦点，失焦时关闭
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

PanelWindow::~PanelWindow() {
    if (m_dragWnd && IsWindow(m_dragWnd)) DestroyWindow(m_dragWnd);
    m_dragWnd = nullptr;
    if (m_hwnd && IsWindow(m_hwnd)) DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    if (m_owner) m_owner->m_panel = nullptr;
}

void PanelWindow::Close() { if (m_hwnd) DestroyWindow(m_hwnd); }

void PanelWindow::DragStart(int idx, POINT clientPt) {
    if (idx < 0 || (size_t)idx >= m_data.shortcuts.size()) return;
    m_draggingOut = true;
    m_dragIdx = idx;
    m_downPt = clientPt;

    int w = util::Scaled(96), h = util::Scaled(88);
    if ((size_t)idx < m_plates.size()) {
        const RECT& plate = m_plates[idx];
        w = plate.right - plate.left;
        h = plate.bottom - plate.top;
    }
    if (w < util::Scaled(48)) w = util::Scaled(48);
    if (h < util::Scaled(48)) h = util::Scaled(48);

    POINT cur; GetCursorPos(&cur);
    m_dragWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                                L"DeskStackDragImage", L"", WS_POPUP,
                                cur.x - w / 2, cur.y - h / 2, w, h,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (m_dragWnd) {
        RECT wr; GetWindowRect(m_dragWnd, &wr);
        int ww = wr.right - wr.left, hh = wr.bottom - wr.top;
        layered::Present(m_dragWnd, ww, hh, wr.left, wr.top,
            [&](Graphics& g, int ww2, int hh2) {
                DrawDragBitmap(g, ww2, hh2, m_data.shortcuts[idx], m_iconSize);
            }, 190);
        ShowWindow(m_dragWnd, SW_SHOW);
    }
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
}

void PanelWindow::DragMove(POINT screenPt) {
    if (!m_dragWnd) return;
    RECT rc; GetWindowRect(m_dragWnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    SetWindowPos(m_dragWnd, HWND_TOPMOST, screenPt.x - w / 2, screenPt.y - h / 2,
                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

void PanelWindow::DragCancel() {
    if (m_dragWnd) { DestroyWindow(m_dragWnd); m_dragWnd = nullptr; }
    m_draggingOut = false;
    m_dragIdx = -1;
}

void PanelWindow::DragEnd(POINT screenPt) {
    if (m_dragWnd) { DestroyWindow(m_dragWnd); m_dragWnd = nullptr; }

    bool dropped = false;
    int idx = m_dragIdx;
    if (idx >= 0 && (size_t)idx < m_data.shortcuts.size()) {
        // 暂时隐藏面板，以便 WindowFromPoint 取到桌面/桌面图标列表窗口。
        // 隐藏/释放捕获可能会触发 WM_CAPTURECHANGED 从而修改 m_dragIdx，
        // 因此后续一律使用局部 idx，避免索引失效。
        m_inDragDrop = true;
        if (m_hwnd && IsWindow(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
        HWND under = WindowFromPoint(screenPt);
        m_inDragDrop = false;
        if (under && desktop::IsDesktopWindow(under)) {
            const ShortcutEntry& e = m_data.shortcuts[idx];
            std::wstring target = e.path;
            if (e.type == "lnk") {
                std::wstring t = desktop::LnkTarget(e.path);
                if (!t.empty()) target = t;
            }
            if (desktop::CreateDesktopShortcut(e.name, target)) {
                if (m_owner) {
                    auto& sc = m_owner->Data().shortcuts;
                    if (idx >= 0 && (size_t)idx < sc.size()) {
                        sc.erase(sc.begin() + idx);
                    }
                    m_owner->Render();
                    Config::MarkDirty();
                }
                dropped = true;
            }
        }
    }

    m_draggingOut = false;
    m_dragIdx = -1;
    if (dropped) {
        Close();   // 成功拖到桌面：关闭面板
    } else if (m_hwnd && IsWindow(m_hwnd)) {
        // 未放到桌面：恢复面板
        ShowWindow(m_hwnd, SW_SHOW);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void PanelWindow::ComputeLayout(std::vector<POINT>& centers) {
    int n = (int)m_data.shortcuts.size();
    if (n <= 0) n = 0;
    centers.clear();

    // 容器屏幕外框中心
    RECT cr = {};
    HWND ctx = m_owner ? m_owner->Hwnd() : nullptr;
    GetWindowRect(ctx, &cr);
    int cex = cr.left + (cr.right - cr.left) / 2;
    int cey = cr.top + (cr.bottom - cr.top) / 2;
    m_screenCenterX = cex;
    m_screenCenterY = cey;

    int iconSize = desktop::IconSize();   // 系统实际图标尺寸
    int pad = util::Scaled(12);

    // 单显示器：取容器所在工作区（多显示器暂按主工作区近似）
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    auto applyWindowAndRatio = [&](int neededW, int neededH, int anchorLeft, int anchorTop) {
        m_windowX = anchorLeft;
        m_windowY = anchorTop;
        if (m_windowX < wa.left) m_windowX = wa.left;
        if (m_windowY < wa.top)  m_windowY = wa.top;
        if (m_windowX + neededW > wa.right)  m_windowX = wa.right - neededW;
        if (m_windowY + neededH > wa.bottom) m_windowY = wa.bottom - neededH;
    };

    if (m_data.style == ExpandStyle::Grid || m_data.style == ExpandStyle::Column ||
        m_data.style == ExpandStyle::Row) {
        // 可用面板最大尺寸（留边，避免溢出屏幕/工作区）
        int maxW = (wa.right - wa.left) - util::Scaled(20);
        int maxH = (wa.bottom - wa.top) - util::Scaled(20);
        if (maxW < util::Scaled(100)) maxW = util::Scaled(100);
        if (maxH < util::Scaled(100)) maxH = util::Scaled(100);

        int cols, rows;
        if (m_data.style == ExpandStyle::Column) { cols = 1; }
        else if (m_data.style == ExpandStyle::Row) { cols = std::max(1, n); }
        else { cols = m_data.gridCols > 0 ? m_data.gridCols : 5; if (n < cols) cols = std::max(1, n); }
        rows = (cols > 0) ? (n + cols - 1) / cols : 0;
        if (rows < 1) rows = 1;

        // 每个条目的“实际区域”（图标+文字包围盒）尺寸
        int labelHx = (int)(util::Scaled(13) + util::Scaled(6));
        int gapV = util::Scaled(6), vpad = util::Scaled(6), hpad = util::Scaled(8);
        int maxPlatW = 0, maxPlatH = 0;
        for (int i = 0; i < n; i++) {
            int lw = LayoutLabelWidth(m_data.shortcuts[i].name.c_str(), util::Scaled(13));
            int lines = WrappedLineCount(m_data.shortcuts[i].name, g_settings.maxChars, g_settings.maxLines);
            int pw = (lw > iconSize ? lw : iconSize) + 2 * hpad;
            int ph = iconSize + gapV + labelHx * lines + 2 * vpad;
            if (pw > maxPlatW) maxPlatW = pw;
            if (ph > maxPlatH) maxPlatH = ph;
        }
        if (maxPlatW < iconSize + 2 * hpad) maxPlatW = iconSize + 2 * hpad;
        if (maxPlatH < iconSize + gapV + labelHx + 2 * vpad) maxPlatH = iconSize + gapV + labelHx + 2 * vpad;

        // 单元格 = 实际区域 + 间距；条目少时间距稍大，多时按需缩小（防溢出）
        int colGap = util::Scaled(10), rowGap = util::Scaled(8);
        int comfCellW = maxPlatW + colGap;
        int comfCellH = maxPlatH + rowGap;
        int naturalW = cols * comfCellW + 2 * pad + colGap;
        int naturalH = rows * comfCellH + 2 * pad + rowGap;

        float scale = 1.0f;
        if (naturalW > maxW || naturalH > maxH) {
            float sx = (float)maxW / (float)naturalW;
            float sy = (float)maxH / (float)naturalH;
            scale = sx < sy ? sx : sy;
            if (scale < 0.35f) scale = 0.35f;   // 数量多时允许更紧凑，贴紧排布
        }
        int cellW = (int)(comfCellW * scale);
        int cellH = (int)(comfCellH * scale);
        // 单元格至少容纳图标本体（不高于此回充，避免面板超出屏幕而“显示不完全”）
        if (cellW < iconSize + util::Scaled(2)) cellW = iconSize + util::Scaled(2);
        if (cellH < iconSize + util::Scaled(2)) cellH = iconSize + util::Scaled(2);

        int neededW = cols * cellW + 2 * pad + colGap;
        int neededH = rows * cellH + 2 * pad + rowGap;
        // 最后一重保险：无论如何不超出工作区，保证所有条目都进入可视面板
        if (neededW > maxW) { double f=(double)(maxW-2*pad-colGap)/(double)cols; if(f<iconSize+1)f=iconSize+1; cellW=(int)f; neededW=cols*cellW+2*pad+colGap; }
        if (neededH > maxH) { double f=(double)(maxH-2*pad-rowGap)/(double)rows; if(f<iconSize+1)f=iconSize+1; cellH=(int)f; neededH=rows*cellH+2*pad+rowGap; }

        m_labelW = maxPlatW - 2 * hpad;   // 文字宽度限于实际区域内
        m_windowW = neededW; m_windowH = neededH;
        // 放在容器右下
        int ax = cr.left + util::Scaled(8);
        int ay = cr.bottom + util::Scaled(8);
        applyWindowAndRatio(neededW, neededH, ax, ay);

        for (int i = 0; i < n; i++) {
            int col = i % cols, row = i / cols;
            int cx = m_windowX + pad + col * cellW + cellW / 2;
            int cy = m_windowY + pad + row * cellH + cellH / 2;
            centers.push_back({ cx, cy });
        }
        m_iconSize = iconSize;
        return;
    }

    // Fan / Ring：扇面宽度(=外-内)固定，外/内半径按图标数量动态调整。
    // 目标：均匀分布、不超出区域、紧凑优先、仅在放不下时放大半径。
    iconSize = desktop::IconSize();
    int nIcons = n;

    int font = util::Scaled(13);
    int labelH = font + util::Scaled(6);
    int vpad = util::Scaled(6);
    int hpad = util::Scaled(8);
    int gapArc = util::Scaled(5);

    // 条目“实际占用宽度/高度”（图标+文字 plate）
    int wMax = iconSize + 2 * hpad;
    int maxPlatH = iconSize + util::Scaled(6) + labelH + 2 * vpad;
    int maxLines = 1;
    for (int i = 0; i < nIcons; i++) {
        int lw = LayoutLabelWidth(m_data.shortcuts[i].name.c_str(), font);
        int wItem = (lw > iconSize ? lw : iconSize) + 2 * hpad;
        if (wItem > wMax) wMax = wItem;
        int lines = WrappedLineCount(m_data.shortcuts[i].name, g_settings.maxChars, g_settings.maxLines);
        if (lines > maxLines) maxLines = lines;
    }
    maxPlatH = iconSize + util::Scaled(6) + labelH * maxLines + 2 * vpad;
    int itemStep = wMax + gapArc;              // 相邻图标中心至少需要达到的弦长
    int topInset = iconSize / 2 + vpad;        // plate 上边距（向内）
    int bottomOut = maxPlatH - topInset;       // plate 下边距（向外）

    // 固定扇带厚度（设计值 260-140=120 逻辑像素），仅当条目高度放不下时才放宽
    int bandT = util::Scaled(260) - util::Scaled(140);
    int minBand = 2 * std::max(topInset, bottomOut) + util::Scaled(8);
    if (bandT < minBand) bandT = minBand;

    // 屏幕约束（最大外层半径）：面板直径约为 2*Rout，尽量占满工作区短边，
    // 但留出窗口边距；这样“放不下才放大半径”时能利用更多屏幕空间，减少末端重叠。
    RECT wa2; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa2, 0);
    int halfDiag = (int)(std::min(wa2.right - wa2.left, wa2.bottom - wa2.top) * 0.50f)
                   - util::Scaled(4);
    if (halfDiag < util::Scaled(60)) halfDiag = util::Scaled(60);

    const double PI = 3.141592653589793;
    int compactR0 = util::Scaled(120);
    int RinFloor  = util::Scaled(40);
    int Rmax = halfDiag - bandT / 2;          // 外层不超出屏幕（尺寸约束）
    int Rmin = RinFloor + bandT / 2;          // 内镂空不小于最小值
    if (Rmax < Rmin) Rmax = Rmin;

    // 仅环形需要额外按“容器中心到屏幕边缘”限制外径，避免外径超出屏幕。
    int ringRmax = Rmax;
    {
        int maxByCenter = (int)std::min({
            cex - wa2.left,
            wa2.right - cex,
            cey - wa2.top,
            wa2.bottom - cey
        });
        int centerRmax = maxByCenter - bandT / 2 - util::Scaled(4);
        if (centerRmax < ringRmax) ringRmax = centerRmax;
        if (ringRmax < Rmin) ringRmax = Rmin;
    }

    int R = compactR0;
    double aDeg = 360.0;

    if (m_data.style == ExpandStyle::Ring) {
        // 环形：按“不重叠”所需半径计算（弦长优先），但保持紧凑半径下限
        double needR = compactR0;
        if (nIcons > 1) {
            double chordNeed = itemStep / (2.0 * std::sin(PI / (double)nIcons));
            double arcNeed = (double)nIcons * itemStep / (2.0 * PI);
            needR = std::max(chordNeed, arcNeed);
        }
        R = (int)(needR + 0.5);
        if (R < Rmin) R = Rmin;
        if (R < compactR0) R = compactR0;
        if (R > ringRmax) R = ringRmax;
        aDeg = 360.0;
    } else {
        // 扇形：紧凑优先。最小 60°，最大 180°。
        // 两端各留出 plate 的角向半宽，保证首尾图标/文字不越出扇形边界。
        R = compactR0;
        if (R < Rmin) R = Rmin;
        if (R > Rmax) R = Rmax;
        for (int iter = 0; iter < 2000; iter++) {
            // 相邻两个中心的最小角间距（由弦长 >= itemStep 推导）
            double chordAng = 0.0;
            if (nIcons > 1) {
                double x = itemStep / (2.0 * R);
                if (x > 1.0) {
                    chordAng = 180.0;   // 当前半径下即使 180° 也放不下
                } else {
                    chordAng = 2.0 * std::asin(x) * 180.0 / PI;
                }
            }

            // aDeg 与边界留白相互依赖，做简单固定点迭代收敛
            double span = 60.0;
            aDeg = 60.0;
            for (int fp = 0; fp < 30; fp++) {
                double start = 270.0 - aDeg / 2.0;
                double boundaryInset = RequiredBoundaryInset(
                    R, start, (double)wMax / 2.0,
                    (double)topInset, (double)bottomOut);
                double need = (nIcons > 1)
                    ? (2.0 * boundaryInset + (nIcons - 1) * chordAng)
                    : (2.0 * boundaryInset);
                if (need < 60.0) need = 60.0;
                span = need;
                double newA = need > 180.0 ? 180.0 : need;
                if (std::fabs(newA - aDeg) < 0.05) { aDeg = newA; break; }
                aDeg = newA;
            }

            if (span <= 180.0 + 1e-6) break;
            R += util::Scaled(4);       // 放不下才放大半径
            if (R > Rmax) { R = Rmax; aDeg = 180.0; break; }
        }
    }

    int Rout = R + bandT / 2;
    int Rin  = R - bandT / 2;
    if (Rin < RinFloor) Rin = RinFloor;

    // 图标+文字的重心比圆心更偏内（文字在图标下方）。
    // 数量少时把条目中心向外移一点，让图标/文字更居中；
    // 数量多时不再外移，而是缩小外径，让扇面外边界更贴近条目，减少外侧留白。
    double itemR = R;
    double outwardShift = (bottomOut - topInset) / 2.0;
    if (nIcons >= 4) {
        outwardShift = 0;
        int shrinkOuter = (bottomOut - topInset) / 2;
        if (shrinkOuter > 0) {
            Rout -= shrinkOuter;
            int minRout = (int)itemR + bottomOut;
            if (Rout < minRout) Rout = minRout;
        }
    } else if (outwardShift > 0) {
        double maxShift = Rout - bottomOut - R;
        if (maxShift < 0) maxShift = 0;
        if (outwardShift > maxShift) outwardShift = maxShift;
        itemR += outwardShift;
    }

    // 外径/内径缩放：仅扇形/环形生效，只影响绘制边界到图标中心的距离
    if (m_data.style == ExpandStyle::Fan || m_data.style == ExpandStyle::Ring) {
        double outerDist = Rout - R;
        double innerDist = R - Rin;
        Rout = R + (int)(outerDist * g_settings.outerScale + 0.5);
        Rin  = R - (int)(innerDist * g_settings.innerScale + 0.5);
        if (Rin < RinFloor) Rin = RinFloor;
        if (Rout < R) Rout = R;
    }

    m_aDeg = aDeg;   // 记录展开角，供背景绘制保持一致
    if (nIcons == 0) {
        centers.clear();
        m_windowX = cex - util::Scaled(40); m_windowY = cey - util::Scaled(40);
        m_windowW = util::Scaled(80); m_windowH = util::Scaled(40);
        m_iconSize = iconSize;
        m_radius = R; m_rout = Rout; m_rin = Rin;
        m_aDeg = (m_data.style == ExpandStyle::Ring) ? 360.0 : 60.0;
        m_labelW = util::Scaled(72);
        return;
    }

    // 均匀分布：
    // Ring 等分 360°（i/n，避免首尾重合）；
    // Fan 在 [270°-aDeg/2, 270°+aDeg/2] 内，两端各留 boundaryInset 后均分。
    double startDeg = (m_data.style == ExpandStyle::Ring) ? 0.0 : (270.0 - aDeg / 2.0);
    if (nIcons == 1) {
        double deg = (m_data.style == ExpandStyle::Ring) ? 0.0 : 270.0;
        double rad = deg * PI / 180.0;
        centers.push_back({ cex + (int)(itemR * std::cos(rad)),
                            cey + (int)(itemR * std::sin(rad)) });
    } else if (m_data.style == ExpandStyle::Ring) {
        for (int i = 0; i < nIcons; i++) {
            double deg = 360.0 * (double)i / (double)nIcons;
            double rad = deg * PI / 180.0;
            centers.push_back({ cex + (int)(itemR * std::cos(rad)),
                                cey + (int)(itemR * std::sin(rad)) });
        }
    } else {
        double boundaryInset = RequiredBoundaryInset(
            R, startDeg, (double)wMax / 2.0,
            (double)topInset, (double)bottomOut);
        double usable = aDeg - 2.0 * boundaryInset;
        if (usable < 0.0) usable = 0.0;
        for (int i = 0; i < nIcons; i++) {
            double deg = startDeg + boundaryInset +
                         usable * (double)i / (double)(nIcons - 1);
            double rad = deg * PI / 180.0;
            centers.push_back({ cex + (int)(itemR * std::cos(rad)),
                                cey + (int)(itemR * std::sin(rad)) });
        }
    }

    // 包围盒按实际 plate 外延与扇面外径计算，保证所有条目/背景完整可见
    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    for (auto& c : centers) {
        minx = std::min(minx, (int)c.x - wMax / 2);
        maxx = std::max(maxx, (int)c.x + wMax / 2);
        miny = std::min(miny, (int)c.y - topInset);
        maxy = std::max(maxy, (int)c.y + bottomOut);
    }
    minx = std::min(minx, cex - Rout); maxx = std::max(maxx, cex + Rout);
    miny = std::min(miny, cey - Rout); maxy = std::max(maxy, cey + Rout);

    int winMargin = util::Scaled(4);
    // 环形专用：面板窗口中心不强制固定为容器中心。
    // 若“圆心+外径”会超出屏幕，则把整个环形（圆心与所有条目）向反方向平移，留 20px 空白。
    if (m_data.style == ExpandStyle::Ring) {
        int ringMargin = util::Scaled(20);
        int left = minx - winMargin;
        int top = miny - winMargin;
        int right = maxx + winMargin;
        int bottom = maxy + winMargin;
        int dx = 0, dy = 0;
        if (left < wa2.left + ringMargin) dx = wa2.left + ringMargin - left;
        else if (right > wa2.right - ringMargin) dx = wa2.right - ringMargin - right;
        if (top < wa2.top + ringMargin) dy = wa2.top + ringMargin - top;
        else if (bottom > wa2.bottom - ringMargin) dy = wa2.bottom - ringMargin - bottom;
        if (dx != 0 || dy != 0) {
            for (auto& c : centers) { c.x += dx; c.y += dy; }
            cex += dx; cey += dy;
            m_screenCenterX = cex;
            m_screenCenterY = cey;
            minx += dx; maxx += dx;
            miny += dy; maxy += dy;
        }
    }

    minx -= winMargin; miny -= winMargin;
    m_windowX = minx; m_windowY = miny;
    int w = maxx - minx + util::Scaled(8), h = maxy - miny + util::Scaled(8);
    // 窗口钳制到工作区内，并保留边距，避免面板贴到屏幕边缘
    if (m_windowX < wa2.left + winMargin) m_windowX = wa2.left + winMargin;
    if (m_windowY < wa2.top + winMargin)  m_windowY = wa2.top + winMargin;
    if (m_windowX + w > wa2.right - winMargin) m_windowX = wa2.right - winMargin - w;
    if (m_windowY + h > wa2.bottom - winMargin) m_windowY = wa2.bottom - winMargin - h;
    m_windowW = w; m_windowH = h;
    m_iconSize = iconSize;
    m_radius = R; m_rout = Rout; m_rin = Rin;
    m_labelW = util::Scaled(72);
}

int PanelWindow::ItemAt(POINT client) const {
    // 使用每个条目“实际区域”（plate 矩形）来命中，与 Windows 桌面图标一致
    for (size_t i = 0; i < m_plates.size(); i++) {
        const RECT& rc = m_plates[i];
        if (client.x >= rc.left && client.x <= rc.right &&
            client.y >= rc.top && client.y <= rc.bottom)
            return (int)i;
    }
    return -1;
}

void PanelWindow::DrawItem(Graphics& g, int idx, int iconSize,
                           const ShortcutEntry& e) {
    if (idx < 0 || (size_t)idx >= m_plates.size()) return;
    const POINT& center = m_centers[idx];
    const RECT& plate  = m_plates[idx];
    int pw = plate.right - plate.left;
    // 图标画在 plate 顶部中央
    int ix = plate.left + pw / 2 - iconSize / 2;
    int iy = plate.top + util::Scaled(4);
    // 图标选择顺序：
    // 1) UWP 快捷方式 → 使用容器自身图标；
    // 2) 其他快捷方式 → 使用 IconBitmapForPath（.ico 直接以 GDI+ 位图加载，保留透明通道）；
    // 3) 失败 → 回退 HICON / 占位图标。
    Bitmap* iconBmp = nullptr;
    if (iconlib::IsUwpShortcut(e.path) && m_owner && !m_owner->Data().iconPath.empty())
        iconBmp = iconlib::IconBitmapForPath(m_owner->Data().iconPath, iconSize);
    else
        iconBmp = iconlib::IconBitmapForPath(e.path, iconSize);

    if (IsUsableIconBitmap(iconBmp)) {
        DrawBitmapIcon(g, iconBmp, ix, iy, iconSize, iconSize);
        delete iconBmp;
    } else {
        if (iconBmp) delete iconBmp;
        HICON ic = iconlib::IconForPath(e.path, iconSize);
        if (ic) {
            DrawHIcon(g, ic, ix, iy, iconSize, iconSize);
            DestroyIcon(ic);
        } else {
            DrawFallbackIcon(g, e.name.c_str(), ix, iy, iconSize, iconSize);
        }
    }
    // 文字画在 plate 下部、居中、宽度限制在 plate 内。
    // 超过 g_settings.maxChars 会换行，最多 g_settings.maxLines 行，第 3 行末尾省略。
    int labelW = pw - util::Scaled(4);
    int gap = util::Scaled(4);
    int lines = WrappedLineCount(e.name, g_settings.maxChars, g_settings.maxLines);
    int labelH = (int)(util::Scaled(13) + util::Scaled(6));
    int ly = iy + iconSize + gap + labelH * lines / 2;   // 文字块垂直中心
    DrawWrappedLabel(g, e.name.c_str(), center.x, ly,
                     (REAL)util::Scaled(13), labelW, Color(255, 0x20, 0x20, 0x20));
}

void PanelWindow::Draw(Graphics& g, int w, int h) {
    DrawPanelBackground(g, w, h);
    // hover：先画出该条目的“实际区域”底色（同 Windows 桌面图标）
    if (m_hoverItem >= 0 && (size_t)m_hoverItem < m_plates.size())
        FillPlate(g, m_plates[m_hoverItem], Color(140, 0xE3, 0xF2, 0xFD));
    if (m_centers.empty() && m_data.shortcuts.empty())
        DrawLabel(g, L"（空）", w / 2, h / 2, (REAL)util::Scaled(13),
                  util::Scaled(120), Color(150, 0x20, 0x20, 0x20));
    for (size_t i = 0; i < m_centers.size() && i < m_data.shortcuts.size(); i++)
        DrawItem(g, (int)i, m_iconSize, m_data.shortcuts[i]);
}

void PanelWindow::DrawPanelBackground(Graphics& g, int w, int h) {
    static const Color bg(210, 255, 255, 255);   // 白半透明 rgba(255,255,255,210)
    if ((m_data.style == ExpandStyle::Fan || m_data.style == ExpandStyle::Ring) &&
        !m_data.shortcuts.empty()) {
        // 使用 ComputeLayout 计算的自适应半径（与图标排布一致，防溢出）
        // 空容器直接走下面的圆角矩形背景，避免为 80x40 的小窗画一个被裁剪的大扇环。
        int Rout = m_rout ? m_rout : util::Scaled(260);
        int Rin  = m_rin  ? m_rin  : util::Scaled(140);
        // 使用 ComputeLayout 算出的展开角（与图标排布一致）
        double aDeg = m_aDeg ? m_aDeg : 360.0;
        if (m_data.style == ExpandStyle::Fan && aDeg <= 0) aDeg = 60.0;
        // GDI+ 角度：以 +x 为 0，顺时针为正；与 ComputeLayout 的图标排布保持一致
        // 开口朝下 ⇒ 扇面弧向上凸，中心沿 -y（270°）
        double startDeg = 270.0 - aDeg / 2.0;
        // GDI+ 中正角度 = 顺时针（沿 y 向下方向增加）
        double s0 = startDeg;                   // 起始角度
        double sweep = aDeg;
        double e0 = (s0 + sweep);
        const double PI = 3.141592653589793;
        double sRad = s0 * PI / 180.0;
        double eRad = e0 * PI / 180.0;
        double psx = m_cx + Rout * std::cos(sRad), psy = m_cy + Rout * std::sin(sRad);
        double pex = m_cx + Rout * std::cos(eRad), pey = m_cy + Rout * std::sin(eRad);
        double isx = m_cx + Rin * std::cos(sRad), isy = m_cy + Rin * std::sin(sRad);
        double iex = m_cx + Rin * std::cos(eRad), iey = m_cy + Rin * std::sin(eRad);

        if (m_data.style == ExpandStyle::Ring) {
            // 环形：内外两个整圆弧 + 一条径向接缝
            GraphicsPath ring;
            ring.AddArc(Rect((int)(m_cx - Rout), (int)(m_cy - Rout),
                             (int)(2 * Rout), (int)(2 * Rout)),
                        (REAL)s0, (REAL)sweep);
            ring.AddLine((REAL)pex, (REAL)pey, (REAL)iex, (REAL)iey);
            ring.AddArc(Rect((int)(m_cx - Rin), (int)(m_cy - Rin),
                             (int)(2 * Rin), (int)(2 * Rin)),
                        (REAL)e0, (REAL)-sweep);
            ring.AddLine((REAL)isx, (REAL)isy, (REAL)psx, (REAL)psy);
            ring.CloseFigure();
            SolidBrush br(bg);
            g.FillPath(&br, &ring);
        } else {
            // 扇形：两侧边为“整条向外弯曲”的贝塞尔弧。
            // 为避免 GraphicsPath 对自交/外凸贝塞尔的填充不完整，
            // 这里采样外弧、外凸侧边、内弧、外凸侧边构成多边形后填充。
            // 侧边采用与内外弧相切的三次贝塞尔：控制点沿弧切线方向放置，
            // 端点处与扇面内外径自然衔接，不出现明显夹角。
            // 为满足“三段式”，下面把该三次贝塞尔按 t=1/3、2/3 分成三段采样。
            float sideLen = (float)(Rout - Rin) * 0.5f;
            // 右侧外凸方向取弧切线方向；左侧外凸方向取弧切线反方向。
            PointF tEnd((REAL)(-std::sin(eRad)), (REAL)(std::cos(eRad)));
            PointF tStart((REAL)(std::sin(sRad)), (REAL)(-std::cos(sRad)));
            PointF cEnd1((REAL)(pex + sideLen * tEnd.X),
                         (REAL)(pey + sideLen * tEnd.Y));
            PointF cEnd2((REAL)(iex + sideLen * tEnd.X),
                         (REAL)(iey + sideLen * tEnd.Y));
            PointF cStart1((REAL)(isx + sideLen * tStart.X),
                           (REAL)(isy + sideLen * tStart.Y));
            PointF cStart2((REAL)(psx + sideLen * tStart.X),
                           (REAL)(psy + sideLen * tStart.Y));
            PointF pEnd((REAL)pex, (REAL)pey);
            PointF pEndInner((REAL)iex, (REAL)iey);
            PointF pStartInner((REAL)isx, (REAL)isy);
            PointF pStart((REAL)psx, (REAL)psy);

            std::vector<PointF> pts;
            const int ARC_STEPS = 48;
            const int BEZ_STEPS_PER_SEG = 12;   // 三段式，每段 12 个采样点
            // 外弧：s0 -> e0
            for (int i = 0; i <= ARC_STEPS; i++) {
                double ang = (s0 + sweep * (double)i / ARC_STEPS) * PI / 180.0;
                pts.push_back(PointF((REAL)(m_cx + Rout * std::cos(ang)),
                                     (REAL)(m_cy + Rout * std::sin(ang))));
            }
            // 侧边 1：外弧末端 -> 内弧末端，向外弯（三段式采样）
            for (int seg = 0; seg < 3; seg++) {
                double t0 = seg / 3.0, t1 = (seg + 1) / 3.0;
                for (int i = 0; i <= BEZ_STEPS_PER_SEG; i++) {
                    double t = t0 + (t1 - t0) * (double)i / BEZ_STEPS_PER_SEG;
                    double mt = 1.0 - t;
                    double x = mt*mt*mt*pEnd.X + 3*mt*mt*t*cEnd1.X + 3*mt*t*t*cEnd2.X + t*t*t*pEndInner.X;
                    double y = mt*mt*mt*pEnd.Y + 3*mt*mt*t*cEnd1.Y + 3*mt*t*t*cEnd2.Y + t*t*t*pEndInner.Y;
                    pts.push_back(PointF((REAL)x, (REAL)y));
                }
            }
            // 内弧：e0 -> s0
            for (int i = 0; i <= ARC_STEPS; i++) {
                double ang = (e0 - sweep * (double)i / ARC_STEPS) * PI / 180.0;
                pts.push_back(PointF((REAL)(m_cx + Rin * std::cos(ang)),
                                     (REAL)(m_cy + Rin * std::sin(ang))));
            }
            // 侧边 2：内弧起点 -> 外弧起点，向外弯（三段式采样）
            for (int seg = 0; seg < 3; seg++) {
                double t0 = seg / 3.0, t1 = (seg + 1) / 3.0;
                for (int i = 0; i <= BEZ_STEPS_PER_SEG; i++) {
                    double t = t0 + (t1 - t0) * (double)i / BEZ_STEPS_PER_SEG;
                    double mt = 1.0 - t;
                    double x = mt*mt*mt*pStartInner.X + 3*mt*mt*t*cStart1.X + 3*mt*t*t*cStart2.X + t*t*t*pStart.X;
                    double y = mt*mt*mt*pStartInner.Y + 3*mt*mt*t*cStart1.Y + 3*mt*t*t*cStart2.Y + t*t*t*pStart.Y;
                    pts.push_back(PointF((REAL)x, (REAL)y));
                }
            }
            SolidBrush br(bg);
            g.FillPolygon(&br, pts.data(), (INT)pts.size());
        }
        return;
    }
    // Grid / Column / Row：白色半透明圆角矩形（12px 圆角）
    FillRounded(g, 0, 0, w, h, util::Scaled(12), bg);
}

void PanelWindow::Render() {
    if (!m_hwnd) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wr; GetWindowRect(m_hwnd, &wr);
    layered::Present(m_hwnd, w, h, wr.left, wr.top,
        [&](Gdiplus::Graphics& gg, int ww, int hh) { Draw(gg, ww, hh); });
}

LRESULT CALLBACK PanelWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PanelWindow* self = (PanelWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && self && !self->m_inDragDrop) {
                self->Close();
                return 0;
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!self) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            self->m_dragIdx = self->ItemAt(pt);
            self->m_downPt = pt;
            self->m_draggingOut = false;
            if (self->m_dragIdx >= 0) SetCapture(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!self) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };

            // 条目上按下并越过拖动阈值：启动“拖到桌面”
            if (self->m_dragIdx >= 0 && !self->m_draggingOut) {
                int dx = GetSystemMetrics(SM_CXDRAG);
                int dy = GetSystemMetrics(SM_CYDRAG);
                if (dx < 2) dx = 2;
                if (dy < 2) dy = 2;
                if (abs(pt.x - self->m_downPt.x) >= dx / 2 ||
                    abs(pt.y - self->m_downPt.y) >= dy / 2) {
                    self->DragStart(self->m_dragIdx, self->m_downPt);
                }
            }
            if (self->m_draggingOut) {
                POINT cp; GetCursorPos(&cp);
                self->DragMove(cp);
                return 0;
            }

            int idx = self->ItemAt(pt);
            if (idx != self->m_hoverItem) {
                self->m_hoverItem = idx;
                self->Render();
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!self) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (self->m_draggingOut && self->m_dragIdx >= 0) {
                // 拖出结束：在光标释放处判断是否放到桌面
                POINT cp; GetCursorPos(&cp);
                self->DragEnd(cp);
                ReleaseCapture();
                return 0;
            }
            int dragIdx = self->m_dragIdx;
            self->m_dragIdx = -1;
            self->m_draggingOut = false;
            ReleaseCapture();
            // 未发生拖动 → 视为单击：打开对应快捷方式
            int idx = (dragIdx >= 0) ? dragIdx : self->ItemAt(pt);
            if (idx >= 0 && (size_t)idx < self->m_data.shortcuts.size()) {
                const auto& e = self->m_data.shortcuts[idx];
                ShellExecuteW(nullptr, L"open", e.path.c_str(), nullptr,
                              e.path.c_str(), SW_SHOWNORMAL);
            }
            self->Close();
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (self && self->m_draggingOut) self->DragCancel();
            if (self) { self->m_dragIdx = -1; }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && self) { self->Close(); return 0; }
            break;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_SETCURSOR: {
            // hover 到快捷方式时显示手指光标
            if (self) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
                BOOL over = self->ItemAt(pt) >= 0;
                SetCursor(LoadCursorW(nullptr, over ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        }
        case WM_MOUSELEAVE:
            if (self && self->m_hoverItem != -1) {
                self->m_hoverItem = -1;
                self->Render();
            }
            return 0;
        case WM_DESTROY:
            if (self) { self->m_hwnd = nullptr; }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
