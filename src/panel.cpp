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

void DrawHIcon(Graphics& g, HICON ic, int x, int y, int w, int h) {
    if (!ic) return;
    Bitmap bmp(ic);
    g.DrawImage(&bmp, x, y, w, h);
}

// 估算文字宽度（中英文混合粗估，够布局用）
int EstimateTextWidth(const WCHAR* text, int fontSize) {
    if (!text) return 0;
    int w = 0;
    for (const WCHAR* p = text; *p; p++) {
        if (*p >= 0x4E00 && *p <= 0x9FFF) w += fontSize;       // 中日韩
        else w += (int)(fontSize * 0.55f);
    }
    return w;
}

// 布局用的文字宽度：限制在合理范围内（过长用省略号），避免间距/半径被长名撑大
int LayoutLabelWidth(const WCHAR* t, int fontSize) {
    int w = EstimateTextWidth(t, fontSize);
    int cap = fontSize * 4;          // 显示区域文字宽度封顶（约 4 字宽）
    return w < cap ? w : cap;
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

} // namespace

void PanelWindow::RegisterClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = PanelWindow::WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DeskStackPanel";
    RegisterClassExW(&wc);
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
    // 面板中心（客户端）＝触发容器屏幕中心 − 面板左上
    m_cx = cex - m_windowX; m_cy = cey - m_windowY;

    // 依据“实际区域”（图标+文字包围盒）构造每个条目的 plate
    m_plates.resize(m_centers.size());
    {
        int labelH = (int)(util::Scaled(13) + util::Scaled(6));
        int vpad = util::Scaled(6), hpad = util::Scaled(8);
        for (size_t i = 0; i < m_centers.size() && i < m_data.shortcuts.size(); i++) {
            int lw = LayoutLabelWidth(m_data.shortcuts[i].name.c_str(), util::Scaled(13));
            m_plates[i] = PlateRect(m_centers[i], m_iconSize, lw, labelH, vpad, hpad);
        }
    }
    Render();
    ShowWindow(m_hwnd, SW_SHOW);
    SetForegroundWindow(m_hwnd);   // 让面板可取得焦点，失焦时关闭
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

PanelWindow::~PanelWindow() {
    if (m_hwnd && IsWindow(m_hwnd)) DestroyWindow(m_hwnd);
    m_hwnd = nullptr;
    if (m_owner) m_owner->m_panel = nullptr;
}

void PanelWindow::Close() { if (m_hwnd) DestroyWindow(m_hwnd); }

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
            int pw = (lw > iconSize ? lw : iconSize) + 2 * hpad;
            int ph = iconSize + gapV + labelHx + 2 * vpad;
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

    // Fan / Ring：扇面宽度(=外-内)固定，外/内半径按图标数量动态调整
    // 目标：均匀分布、不超出区域
    iconSize = desktop::IconSize();
    int nIcons = n;

    // 固定扇带厚度（设计值 260-140=120 逻辑像素）
    int bandT = util::Scaled(260) - util::Scaled(140);
    if (bandT < util::Scaled(40)) bandT = util::Scaled(40);

    // 条目“实际占用宽度”（图标+文字 plate 宽度）
    int hpad = util::Scaled(8), gapArc = util::Scaled(5);
    int wMax = 0;
    for (int i = 0; i < nIcons; i++) {
        int lw = LayoutLabelWidth(m_data.shortcuts[i].name.c_str(), util::Scaled(13));
        int wItem = (lw > iconSize ? lw : iconSize) + 2 * hpad;
        if (wItem > wMax) wMax = wItem;
    }
    if (wMax < iconSize + 2 * hpad) wMax = iconSize + 2 * hpad;
    double neededArc = (double)nIcons * (wMax + gapArc);   // 铺开所需总弧长

    // 屏幕约束（最大外层半径）
    RECT wa2; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa2, 0);
    int halfDiag = (int)(std::min(wa2.right - wa2.left, wa2.bottom - wa2.top) * 0.40f);

    // 紧凑优先：从一个小半径开始，仅当放不下时才逐步放大半径
    const double PI = 3.141592653589793;
    int compactR0 = util::Scaled(120);
    int RinFloor  = util::Scaled(40);
    double aDeg = (m_data.style == ExpandStyle::Ring) ? 360.0 : 60.0;  // 默认最小角

    int R = compactR0;
    if (m_data.style == ExpandStyle::Ring) {
        // 环形：直接按“不重叠”所需半径（用弦长更准确）计算，但要优先紧凑
        double needArcR  = neededArc / (2.0 * PI);                       // 按弧长
        double needChordR = (nIcons > 1) ? (wMax + gapArc) / (2.0 * std::sin(PI / (double)nIcons)) : 0.0; // 按相邻弦长
        double need = needArcR > needChordR ? needArcR : needChordR;
        R = (int)(need + 0.5);
        if (R < compactR0) R = compactR0;
        if (R > halfDiag) R = halfDiag;
    } else {
        // 扇形：均匀铺开在 [60°,180°]，从紧凑半径起，放不下才放大
        for (int iter = 0; iter < 2000; iter++) {
            double needDeg = neededArc / R * 180.0 / PI;   // 该半径下能放下的最小角度
            if (needDeg < 60.0) needDeg = 60.0;
            if (needDeg > 180.0) needDeg = 180.0;
            aDeg = needDeg;
            double availArc = R * aDeg * PI / 180.0;
            if (availArc + 1e-3 >= neededArc) break;       // 当前半径放得下
            R += util::Scaled(2);                          // 放不下才放大半径
            if (R > halfDiag) { R = halfDiag; break; }
        }
    }

    // Rout/Rin 一律由 nIcons 推导出的中心半径 R 决定，带宽 bandT 恒定：
    //   Rout = R + bandT/2 ,  Rin = R - bandT/2
    // 只对 R 做必要的屏幕/最小镂空约束（不破坏 nIcons 决定的比例）
    int Rmax = halfDiag - bandT / 2;          // 外层不超出屏幕
    int Rmin = RinFloor + bandT / 2;          // 内镂空不小于最小值
    if (R > Rmax) R = Rmax;
    if (R < Rmin) R = Rmin;
    if (R < util::Scaled(40)) R = util::Scaled(40);
    int Rout = R + bandT / 2;
    int Rin  = R - bandT / 2;
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

    // 均匀分布：Ring 等分 360°，Fan 在 [270°-aDeg/2, 270°+aDeg/2] 等分（开口朝下）
    double startDeg = (m_data.style == ExpandStyle::Ring) ? 0.0 : (270.0 - aDeg / 2.0);
    for (int i = 0; i < nIcons; i++) {
        double deg = (nIcons > 1) ? (startDeg + aDeg * (double)i / (nIcons - 1))
                                  : (startDeg + aDeg / 2.0);
        double rad = deg * 3.141592653589793 / 180.0;
        int cx = cex + (int)(R * std::cos(rad));
        int cy = cey + (int)(R * std::sin(rad));
        centers.push_back({ cx, cy });
    }

    // 图标+文字都落在 [Rin, Rout] 带内，面板包围盒按实际外延计算，不超出扇面
    int half = std::max(iconSize / 2 + util::Scaled(26), util::Scaled(30)) + (Rout - R);
    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
    for (auto& c : centers) {
        minx = std::min(minx, (int)c.x - half); maxx = std::max(maxx, (int)c.x + half);
        miny = std::min(miny, (int)c.y - half); maxy = std::max(maxy, (int)c.y + half);
    }
    minx = std::min(minx, cex - Rout); maxx = std::max(maxx, cex + Rout);
    miny = std::min(miny, cey - Rout); maxy = std::max(maxy, cey + Rout);
    minx -= util::Scaled(4); miny -= util::Scaled(4);
    m_windowX = minx; m_windowY = miny;
    int w = maxx - minx + util::Scaled(8), h = maxy - miny + util::Scaled(8);
    if (m_windowX < wa2.left) { int dx = wa2.left - m_windowX; m_windowX += dx; }
    if (m_windowY < wa2.top)  { int dy = wa2.top - m_windowY; m_windowY += dy; }
    if (m_windowX + w > wa2.right) { m_windowX = wa2.right - w; }
    if (m_windowY + h > wa2.bottom) { m_windowY = wa2.bottom - h; }
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
    HICON ic = iconlib::IconForPath(e.path, iconSize);
    if (ic) {
        DrawHIcon(g, ic, ix, iy, iconSize, iconSize);
        DestroyIcon(ic);
    } else {
        DrawFallbackIcon(g, e.name.c_str(), ix, iy, iconSize, iconSize);
    }
    // 文字画在 plate 下部、居中、宽度限制在 plate 内(省略号)，避免和图标重叠/溢出
    int labelW = pw - util::Scaled(4);
    int gap = util::Scaled(4);
    int ly = iy + iconSize + gap + util::Scaled(13) / 2;   // 文字行中心
    DrawLabel(g, e.name.c_str(), center.x, ly,
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
    if (m_data.style == ExpandStyle::Fan || m_data.style == ExpandStyle::Ring) {
        // 使用 ComputeLayout 计算的自适应半径（与图标排布一致，防溢出）
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
        // 圆环带（内外两个圆弧 + 两端连接）
        GraphicsPath ring;
        // 外弧（顺时针扫过整段）
        ring.AddArc(Rect((int)(m_cx - Rout), (int)(m_cy - Rout),
                         (int)(2 * Rout), (int)(2 * Rout)),
                    (REAL)s0, (REAL)sweep);
        // 连接外弧末端 → 内弧末端
        double e0 = (s0 + sweep);
        double sRad = s0 * 3.141592653589793 / 180.0;
        double eRad = e0 * 3.141592653589793 / 180.0;
        double psx = m_cx + Rout * std::cos(sRad), psy = m_cy + Rout * std::sin(sRad);
        double pex = m_cx + Rout * std::cos(eRad), pey = m_cy + Rout * std::sin(eRad);
        double isx = m_cx + Rin * std::cos(sRad), isy = m_cy + Rin * std::sin(sRad);
        double iex = m_cx + Rin * std::cos(eRad), iey = m_cy + Rin * std::sin(eRad);
        if (m_data.style == ExpandStyle::Ring) {
            // 用一条微弧/直线连接（360° 时起止点重合，直线退化为点）
            ring.AddLine((int)pex, (int)pey, (int)iex, (int)iey);
            // 内弧（反向，从 e0 到 s0）
            ring.AddArc(Rect((int)(m_cx - Rin), (int)(m_cy - Rin),
                             (int)(2 * Rin), (int)(2 * Rin)),
                        (REAL)e0, (REAL)-sweep);
            // 连接内弧起点 → 外弧起点
            ring.AddLine((int)isx, (int)isy, (int)psx, (int)psy);
        } else {
            // Fan 扇面两侧“整条向外弯曲成弧”：沿半径为 Rin..Rout 之间的切线过渡。
            // 用贝塞尔逼近弯曲边，让两侧呈外凸弧线（符合需求 3.5）
            // 起始边：从外弧起点 (psx,psy) 到内弧起点 (isx,isy)
            // 终点边：从内弧终点 (iex,iey) 到外弧终点 (pex,pey)
            PointF p1((REAL)psx, (REAL)psy), p2((REAL)pex, (REAL)pey);
            PointF q1((REAL)isx, (REAL)isy), q2((REAL)iex, (REAL)iey);
            // 控制点取在中点并沿径向向外推，形成外凸弧
            PointF c1((REAL)((psx+isx)/2.0f), (REAL)((psy+isy)/2.0f));
            PointF c2((REAL)((pex+iex)/2.0f), (REAL)((pey+iey)/2.0f));
            // 沿径向向外推控制点
            double c1Rad = atan2(psy - m_cy, psx - m_cx);
            double c2Rad = atan2(pey - m_cy, pex - m_cx);
            float push = (Rout - Rin) * util::DpiScale() * 0.5f;
            c1.X = (REAL)(m_cx + (Rout + push) * std::cos(c1Rad));
            c1.Y = (REAL)(m_cy + (Rout + push) * std::sin(c1Rad));
            c2.X = (REAL)(m_cx + (Rout + push) * std::cos(c2Rad));
            c2.Y = (REAL)(m_cy + (Rout + push) * std::sin(c2Rad));
            ring.AddBezier(p1, c1, c1, q1);   // 起始弯曲边
            // 内弧
            ring.AddArc(Rect((int)(m_cx - Rin), (int)(m_cy - Rin),
                             (int)(2 * Rin), (int)(2 * Rin)),
                        (REAL)e0, (REAL)-sweep);
            ring.AddBezier(q2, c2, c2, p2);   // 终点弯曲边
        }
        ring.CloseFigure();
        SolidBrush br(bg);
        g.FillPath(&br, &ring);
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
            if (LOWORD(wp) == WA_INACTIVE && self) { self->Close(); return 0; }
            return 0;
        case WM_LBUTTONUP: {
            // 单击快捷方式即打开
            if (!self) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = self->ItemAt(pt);
            if (idx >= 0 && (size_t)idx < self->m_data.shortcuts.size()) {
                const auto& e = self->m_data.shortcuts[idx];
                ShellExecuteW(nullptr, L"open", e.path.c_str(), nullptr,
                              e.path.c_str(), SW_SHOWNORMAL);
            }
            self->Close();
            return 0;
        }
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
        case WM_MOUSEMOVE: {
            if (!self) break;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int idx = self->ItemAt(pt);
            if (idx != self->m_hoverItem) {
                self->m_hoverItem = idx;
                self->Render();
            }
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
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
