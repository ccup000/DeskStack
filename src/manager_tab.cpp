#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#ifndef _WIN32_IE
#define _WIN32_IE 0x0500
#endif
#include "manager_tab.h"
#include "appstate.h"
#include "types.h"
#include "config.h"
#include "container.h"
#include "iconlib.h"
#include "desktop.h"
#include "resource.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <cwchar>
#include <algorithm>

namespace {

// 控件 ID
enum {
    IDC_NAME_EDIT            = 1000,
    IDC_OPEN_MODE_TOGGLE     = 1001,
    IDC_STYLE_COMBO          = 1002,
    IDC_ICON_PREVIEW         = 1003,
    IDC_CHANGE_ICON_BTN      = 1004,
    IDC_ADD_SHORTCUT_BTN     = 1005,
    IDC_DELETE_CONTAINER_BTN = 1006,
    IDC_MODE_SWITCH          = 1007,

    IDC_REMOVE_SHORTCUT_BASE = 4900,
    IDC_SHORTCUT_LABEL_BASE  = 5000,

    IDC_SLIDER_MAX   = 3001,
    IDC_SLIDER_OUTER = 3002,
    IDC_SLIDER_INNER = 3003,
    IDC_SLIDER_LINES = 3004,
    IDC_VALUE_MAX    = 3101,
    IDC_VALUE_OUTER  = 3102,
    IDC_VALUE_INNER  = 3103,
    IDC_VALUE_LINES  = 3104,
    IDC_SETTING_EXPAND_BASE = 3200,
    IDC_ORIGINAL_EXPAND     = 3204,

    IDC_NOTE_BASE    = 6000,
};

const int kNavItemH = 38;
const int kNavGap = 7;
const int kNavMargin = 12;
const int kNavHeaderY = 68;
const int kNavIconSize = 32;

void FillRoundRect(HDC hdc, const RECT& rc, int radius, HBRUSH brush) {
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, brush);
    HPEN oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius * 2, radius * 2);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
}

std::wstring FormatDouble(double v) {
    wchar_t buf[32] = {};
    std::swprintf(buf, 32, L"%.1f", v);
    return buf;
}

std::wstring MakeFilter(const wchar_t* a, const wchar_t* pa,
                       const wchar_t* b, const wchar_t* pb) {
    std::wstring f;
    f += a; f.push_back(L'\0'); f += pa; f.push_back(L'\0');
    f += b; f.push_back(L'\0'); f += pb; f.push_back(L'\0');
    return f;
}

bool PickIconFile(HWND owner, std::wstring& out) {
    std::wstring filter = MakeFilter(L"图标文件 (*.ico)", L"*.ico",
                                     L"程序 (*.exe)", L"*.exe");
    OPENFILENAMEW ofn = { sizeof(ofn) };
    wchar_t file[MAX_PATH] = {};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择容器图标";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_NODEREFERENCELINKS;
    if (GetOpenFileNameW(&ofn) && file[0]) { out = file; return true; }
    return false;
}

bool PickPath(HWND owner, std::wstring& out, bool& isFolder) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择要收纳的文件夹（取消则选择文件）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t buf[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, buf)) { out = buf; isFolder = true; }
        CoTaskMemFree(pidl);
        return isFolder;
    }
    std::wstring filter = MakeFilter(L"所有文件 (*.*)", L"*.*",
                                     L"快捷方式 (*.lnk)", L"*.lnk");
    OPENFILENAMEW ofn = { sizeof(ofn) };
    wchar_t file[MAX_PATH] = {};
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_NODEREFERENCELINKS;
    if (GetOpenFileNameW(&ofn)) { out = file; isFolder = false; return true; }
    return false;
}

void RestoreShortcutToSource(const ShortcutEntry& e) {
    if (e.type != "lnk" || e.mode != ShortcutMode::Original)
        return;
    std::wstring src = e.sourcePath;
    if (src.empty())
        src = desktop::DesktopFolder() + L"\\" + e.name + L".lnk";
    if (!e.path.empty() && GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES)
        CopyFileW(e.path.c_str(), src.c_str(), FALSE);
}

} // namespace

// 管理界面唯一窗口句柄
static HWND g_managerWindow = nullptr;

// ── 静态入口 ────────────────────────────────────────────────────────
void OpenManagerTabWindow(HWND owner) {
    ManageTab::Open(owner);
}

void ManageTab::Open(HWND owner) {
    if (g_managerWindow && IsWindow(g_managerWindow)) {
        ShowWindow(g_managerWindow, SW_RESTORE);
        SetForegroundWindow(g_managerWindow);
        return;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = ManageTab::WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"DeskStackManageTab";
        RegisterClassExW(&wc);

        WNDCLASSEXW nav = { sizeof(nav) };
        nav.lpfnWndProc = ManageTab::NavPaneProc;
        nav.hInstance = hInst;
        nav.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        nav.lpszClassName = L"DeskStackManageNav";
        RegisterClassExW(&nav);

        WNDCLASSEXW right = { sizeof(right) };
        right.lpfnWndProc = ManageTab::RightPaneProc;
        right.hInstance = hInst;
        right.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        right.lpszClassName = L"DeskStackManageRight";
        RegisterClassExW(&right);

        WNDCLASSEXW panel = { sizeof(panel) };
        panel.lpfnWndProc = ManageTab::PanelProc;
        panel.hInstance = hInst;
        panel.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        panel.lpszClassName = L"DeskStackManagePanel";
        RegisterClassExW(&panel);

        registered = true;
    }

    int w = util::Scaled(960);
    int h = util::Scaled(640);
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"DeskStackManageTab", L"DeskStack 管理界面",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                owner, nullptr, hInst, (LPVOID)owner);
    if (!hwnd) return;
    g_managerWindow = hwnd;

    {
        RECT wr; GetWindowRect(hwnd, &wr);
        int cx = (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2;
        int cy = (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2;
        SetWindowPos(hwnd, nullptr, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ── 构造 / 析构 ─────────────────────────────────────────────────────
ManageTab::ManageTab(HWND owner) : m_owner(owner) {
    HDC hdc = GetDC(nullptr);
    int dpiY = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(nullptr, hdc);

    auto makeFont = [&](int pt, int weight) {
        return CreateFontW(-MulDiv(pt, dpiY, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    };

    m_fontNavTitle = makeFont(16, FW_SEMIBOLD);
    m_fontNavItem  = makeFont(10, FW_NORMAL);
    m_fontPanelTitle = makeFont(14, FW_SEMIBOLD);
    m_fontText     = makeFont(10, FW_NORMAL);
    m_fontSmall    = makeFont(9, FW_NORMAL);

    m_whiteBrush = CreateSolidBrush(RGB(255, 255, 255));
    m_navBrush   = CreateSolidBrush(RGB(243, 243, 243));
    m_hoverBrush = CreateSolidBrush(RGB(235, 235, 235));
    m_selBrush   = CreateSolidBrush(RGB(226, 226, 226));
}

ManageTab::~ManageTab() {
    for (HICON ic : m_navIcons)
        if (ic) DestroyIcon(ic);
    m_navIcons.clear();
    if (m_previewIcon) { DestroyIcon(m_previewIcon); m_previewIcon = nullptr; }
    if (m_fontNavTitle) DeleteObject(m_fontNavTitle);
    if (m_fontNavItem) DeleteObject(m_fontNavItem);
    if (m_fontPanelTitle) DeleteObject(m_fontPanelTitle);
    if (m_fontText) DeleteObject(m_fontText);
    if (m_fontSmall) DeleteObject(m_fontSmall);
    if (m_whiteBrush) DeleteObject(m_whiteBrush);
    if (m_navBrush) DeleteObject(m_navBrush);
    if (m_hoverBrush) DeleteObject(m_hoverBrush);
    if (m_selBrush) DeleteObject(m_selBrush);
}

std::wstring ManageTab::ExePath() const {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

void ManageTab::LoadNavIcons() {
    for (HICON ic : m_navIcons)
        if (ic) DestroyIcon(ic);
    m_navIcons.clear();
    for (size_t i = 0; i < g_containers.size(); i++) {
        const auto& d = g_containers[i]->Data();
        std::wstring src = d.iconPath.empty() ? ExePath() : d.iconPath;
        m_navIcons.push_back(iconlib::IconForPath(src, kNavIconSize));
    }
}

// ── 主窗口过程 ──────────────────────────────────────────────────────
LRESULT CALLBACK ManageTab::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ManageTab* self = (ManageTab*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (msg == WM_NCCREATE) {
        auto cs = (CREATESTRUCTW*)lp;
        self = new ManageTab((HWND)cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->m_hwnd = hwnd;
        return TRUE;
    }

    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE:
            self->OnCreate();
            return 0;
        case WM_SIZE:
            self->OnSize();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (g_managerWindow == hwnd)
                g_managerWindow = nullptr;
            delete self;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, self->m_whiteBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            self->ScrollBy(-(delta / WHEEL_DELTA) * util::Scaled(48));
            return 0;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = util::Scaled(720);
            mmi->ptMinTrackSize.y = util::Scaled(480);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ManageTab::OnCreate() {
    RECT rc; GetClientRect(m_hwnd, &rc);
    int navW = util::Scaled(200);

    m_nav = CreateWindowExW(0, L"DeskStackManageNav", L"",
                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                            0, 0, navW, rc.bottom, m_hwnd, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(m_nav, GWLP_USERDATA, (LONG_PTR)this);

    m_right = CreateWindowExW(0, L"DeskStackManageRight", L"",
                              WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL,
                              navW, 0, rc.right - navW, rc.bottom,
                              m_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(m_right, GWLP_USERDATA, (LONG_PTR)this);

    if (!g_containers.empty()) m_selected = 0;
    else m_selected = 0; // 没有容器时也允许先看设置，下面刷新右侧时会判断

    LoadNavIcons();
    RefreshRightPane();
}

void ManageTab::OnSize() {
    if (!m_nav || !m_right) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    int navW = util::Scaled(200);
    SetWindowPos(m_nav, nullptr, 0, 0, navW, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_right, nullptr, navW, 0, (rc.right - navW > 0 ? rc.right - navW : 0), rc.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(m_nav, nullptr, TRUE);
    InvalidateRect(m_right, nullptr, TRUE);
    LayoutPanels();
}

// ── 左侧导航 ────────────────────────────────────────────────────────
void ManageTab::InvalidateNav() const {
    if (m_nav) InvalidateRect(m_nav, nullptr, TRUE);
}

int ManageTab::NavHitTest(POINT pt) const {
    RECT rc; GetClientRect(m_nav, &rc);
    if (pt.x < 0 || pt.x >= rc.right || pt.y < 0 || pt.y >= rc.bottom)
        return -1;

    int x = kNavMargin;
    int w = rc.right - kNavMargin * 2;
    int y = kNavHeaderY;
    for (size_t i = 0; i < g_containers.size(); i++) {
        RECT item = { x, y, x + w, y + kNavItemH };
        if (pt.y >= item.top && pt.y < item.bottom) return (int)i;
        y += kNavItemH + kNavGap;
    }

    // “新增容器tab”按钮
    RECT add = { x, y + 8, x + w, y + 8 + kNavItemH };
    if (pt.y >= add.top && pt.y < add.bottom) return -2;

    // 左下角“设置”
    RECT set = { x, rc.bottom - kNavItemH - 12, x + w, rc.bottom - 12 };
    if (pt.y >= set.top && pt.y < set.bottom) return (int)g_containers.size();

    return -1;
}

void ManageTab::DrawNavItem(HDC hdc, const RECT& rc, const wchar_t* text,
                            bool selected, bool hover, bool accent, HICON icon) {
    HBRUSH bg = selected ? m_selBrush : (hover ? m_hoverBrush : nullptr);
    if (bg) FillRoundRect(hdc, rc, 6, bg);

    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(hdc, m_fontNavItem);
    COLORREF oldColor = SetTextColor(hdc, accent ? RGB(0, 103, 192)
                                                 : (selected ? RGB(0, 0, 0) : RGB(32, 32, 32)));

    RECT tr = rc;
    int leftPad = kNavMargin + 4;
    if (selected) {
        // 当前 active 项前面的绿点
        int dotR = util::Scaled(4);
        int cx = rc.left + kNavMargin + dotR;
        int cy = (rc.top + rc.bottom) / 2;
        HBRUSH green = CreateSolidBrush(RGB(16, 185, 129));
        HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, green);
        HPEN oldPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, cx - dotR, cy - dotR, cx + dotR, cy + dotR);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        DeleteObject(green);

        leftPad = kNavMargin + dotR * 2 + 6;
    }

    if (icon) {
        int iconX = rc.left + leftPad;
        int iconY = rc.top + (rc.bottom - rc.top - kNavIconSize) / 2;
        DrawIconEx(hdc, iconX, iconY, icon, kNavIconSize, kNavIconSize, 0, nullptr, DI_NORMAL);
        leftPad += kNavIconSize + 6;
    }

    tr.left = rc.left + leftPad;
    tr.right -= kNavMargin;
    DrawTextW(hdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SetTextColor(hdc, oldColor);
    SelectObject(hdc, oldFont);
}

void ManageTab::DrawNav(HDC hdc, const RECT& rc) {
    FillRect(hdc, &rc, m_navBrush);

    if (m_navIcons.size() != g_containers.size())
        LoadNavIcons();

    // “容器列表”左右居中，与下方 tab 按钮间距 20px；加大标题区避免底部裁切
    RECT titleRc = { 0, 6, rc.right, 48 };
    SetBkMode(hdc, TRANSPARENT);
    HFONT old = (HFONT)SelectObject(hdc, m_fontNavTitle);
    SetTextColor(hdc, RGB(27, 27, 27));
    DrawTextW(hdc, L"容器列表", -1, &titleRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);

    int x = kNavMargin;
    int w = rc.right - kNavMargin * 2;
    int y = kNavHeaderY;

    for (size_t i = 0; i < g_containers.size(); i++) {
        RECT item = { x, y, x + w, y + kNavItemH };
        bool selected = ((int)i == m_selected);
        bool hover = (m_navHover == (int)i);
        HICON icon = (i < m_navIcons.size()) ? m_navIcons[i] : nullptr;
        DrawNavItem(hdc, item, g_containers[i]->Data().name.c_str(), selected, hover, false, icon);
        y += kNavItemH + kNavGap;
    }

    // 新增容器tab
    RECT add = { x, y + 8, x + w, y + 8 + kNavItemH };
    DrawNavItem(hdc, add, L"+ 新增容器", false, m_navHover == -2, true);

    // 左下角设置
    RECT set = { x, rc.bottom - kNavItemH - 12, x + w, rc.bottom - 12 };
    bool setSelected = (m_selected == (int)g_containers.size());
    DrawNavItem(hdc, set, L"⚙ 设置", setSelected, m_navHover == (int)g_containers.size(), false);

    // 设置上方分隔线
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(226, 226, 226));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, kNavMargin, set.top - 8, nullptr);
    LineTo(hdc, rc.right - kNavMargin, set.top - 8);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void ManageTab::DrawOpenModeSwitch(HDC hdc, const RECT& rc, bool rightActive) const {
    // 开关样式：左侧“悬停”，右侧“点击”
    HBRUSH trackBrush = CreateSolidBrush(RGB(235, 235, 235));
    FillRoundRect(hdc, rc, (rc.bottom - rc.top) / 2, trackBrush);
    DeleteObject(trackBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(205, 205, 205));
    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    int r = (rc.bottom - rc.top) / 2;
    RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, r * 2, r * 2);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    // 当前激活的半边用白色“滑块”填充
    RECT half = rc;
    int mid = rc.left + (rc.right - rc.left) / 2;
    if (rightActive) {
        half.left = mid;
    } else {
        half.right = mid;
    }
    InflateRect(&half, -2, -2);
    FillRoundRect(hdc, half, (half.bottom - half.top) / 2, m_whiteBrush);

    // 文字
    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(hdc, m_fontText);
    RECT leftRc = rc;
    leftRc.right = mid;
    RECT rightRc = rc;
    rightRc.left = mid;

    COLORREF oldColor = SetTextColor(hdc, rightActive ? RGB(130, 130, 130) : RGB(27, 27, 27));
    DrawTextW(hdc, L"悬停", -1, &leftRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, rightActive ? RGB(27, 27, 27) : RGB(130, 130, 130));
    DrawTextW(hdc, L"点击", -1, &rightRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, oldColor);
    SelectObject(hdc, oldFont);
}

void ManageTab::DrawShortcutModeSwitch(HDC hdc, const RECT& rc, bool original) const {
    HBRUSH trackBrush = CreateSolidBrush(RGB(235, 235, 235));
    FillRoundRect(hdc, rc, (rc.bottom - rc.top) / 2, trackBrush);
    DeleteObject(trackBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(205, 205, 205));
    HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    int r = (rc.bottom - rc.top) / 2;
    RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, r * 2, r * 2);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);

    RECT half = rc;
    int mid = rc.left + (rc.right - rc.left) / 2;
    if (original) {
        half.right = mid;   // 原件在左侧，激活左侧
    } else {
        half.left = mid;    // 引用在右侧，激活右侧
    }
    InflateRect(&half, -2, -2);
    FillRoundRect(hdc, half, (half.bottom - half.top) / 2, m_whiteBrush);

    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(hdc, m_fontText);
    RECT leftRc = rc;
    leftRc.right = mid;
    RECT rightRc = rc;
    rightRc.left = mid;

    COLORREF oldColor = SetTextColor(hdc, original ? RGB(27, 27, 27) : RGB(130, 130, 130));
    DrawTextW(hdc, L"原件", -1, &leftRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, original ? RGB(130, 130, 130) : RGB(27, 27, 27));
    DrawTextW(hdc, L"引用", -1, &rightRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, oldColor);
    SelectObject(hdc, oldFont);
}

void ManageTab::DrawShortcutLabel(HDC hdc, const DRAWITEMSTRUCT* dis) const {
    FillRect(hdc, &dis->rcItem, m_whiteBrush);
    wchar_t text[1024] = {};
    GetWindowTextW(dis->hwndItem, text, 1024);

    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(hdc, m_fontText);
    COLORREF oldColor = SetTextColor(hdc, RGB(27, 27, 27));
    RECT rc = dis->rcItem;
    rc.left += util::Scaled(2);
    rc.right -= util::Scaled(2);
    // DT_PATH_ELLIPSIS：路径过长时从中间省略，尽量保留末尾文件名/链接名
    DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SetTextColor(hdc, oldColor);
    SelectObject(hdc, oldFont);
}

void ManageTab::DrawDeleteButton(HDC hdc, const RECT& rc) const {
    FillRoundRect(hdc, rc, 6, m_whiteBrush);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(220, 80, 80));
    HPEN oldPen = (HPEN)SelectObject(hdc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, 8, 8);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);

    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = (HFONT)SelectObject(hdc, m_fontText);
    COLORREF oldColor = SetTextColor(hdc, RGB(200, 50, 50));
    DrawTextW(hdc, L"删除容器", -1, (LPRECT)&rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SetTextColor(hdc, oldColor);
    SelectObject(hdc, oldFont);
}

LRESULT CALLBACK ManageTab::NavPaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ManageTab* self = (ManageTab*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            self->DrawNav(hdc, rc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int hit = self->NavHitTest(pt);
            if (hit != self->m_navHover) {
                self->m_navHover = hit;
                self->InvalidateNav();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->m_navHover = -1;
            self->InvalidateNav();
            return 0;
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int hit = self->NavHitTest(pt);
            if (hit == -2)
                self->OnAddContainer();
            else if (hit >= 0 && hit <= (int)g_containers.size())
                self->OnSelectTab(hit);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
                if (self->NavHitTest(pt) != -1) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 右侧滚动区域 ────────────────────────────────────────────────────
LRESULT CALLBACK ManageTab::RightPaneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ManageTab* self = (ManageTab*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, self->m_whiteBrush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            self->LayoutPanels();
            return 0;
        case WM_VSCROLL:
            switch (LOWORD(wp)) {
                case SB_LINEUP: self->ScrollBy(-util::Scaled(24)); break;
                case SB_LINEDOWN: self->ScrollBy(util::Scaled(24)); break;
                case SB_PAGEUP: self->ScrollBy(-util::Scaled(120)); break;
                case SB_PAGEDOWN: self->ScrollBy(util::Scaled(120)); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: self->ScrollTo(HIWORD(wp)); break;
                case SB_TOP: self->ScrollTo(0); break;
                case SB_BOTTOM: self->ScrollTo(self->m_contentH); break;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            self->ScrollBy(-(delta / WHEEL_DELTA) * util::Scaled(48));
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ManageTab::ScrollBy(int delta) { ScrollTo(m_scrollPos + delta); }

void ManageTab::ScrollTo(int pos) {
    RECT rc; GetClientRect(m_right, &rc);
    int page = rc.bottom - rc.top;
    int max = std::max(0, m_contentH - page);
    pos = std::max(0, std::min(pos, max));
    if (pos == m_scrollPos) return;
    m_scrollPos = pos;
    SCROLLINFO si = { sizeof(si), SIF_POS };
    si.nPos = pos;
    SetScrollInfo(m_right, SB_VERT, &si, TRUE);
    LayoutPanels();
    InvalidateRect(m_right, nullptr, TRUE);
}

void ManageTab::UpdateScroll() {
    RECT rc; GetClientRect(m_right, &rc);
    int page = rc.bottom - rc.top;
    int max = std::max(0, m_contentH - page);
    m_scrollPos = std::max(0, std::min(m_scrollPos, max));
    SCROLLINFO si = { sizeof(si) };
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = std::max(0, m_contentH - 1);
    si.nPage = page;
    si.nPos = m_scrollPos;
    SetScrollInfo(m_right, SB_VERT, &si, TRUE);
}

void ManageTab::LayoutPanels() {
    if (!m_right) return;
    RECT rc; GetClientRect(m_right, &rc);
    int margin = util::Scaled(20);
    int width = rc.right - rc.left - margin * 2;
    if (width < util::Scaled(200)) width = util::Scaled(200);
    int y = margin;

    for (size_t i = 0; i < m_panels.size(); i++) {
        HWND panel = m_panels[i];
        int h = (i < m_panelHeights.size()) ? m_panelHeights[i] : 100;
        SetWindowPos(panel, nullptr, margin, y - m_scrollPos, width, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        ResizePanelChildren(panel, width);
        y += h + margin;
    }
    m_contentH = y;
    UpdateScroll();
}

// ── 右侧 panel 窗口过程 ─────────────────────────────────────────────
LRESULT CALLBACK ManageTab::PanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ManageTab* self = (ManageTab*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);

            FillRect(hdc, &rc, self->m_whiteBrush);

            wchar_t title[256] = {};
            GetWindowTextW(hwnd, title, 256);
            RECT titleRc = { util::Scaled(24), util::Scaled(16), rc.right - util::Scaled(24), util::Scaled(48) };
            SetBkMode(hdc, TRANSPARENT);
            HFONT oldFont = (HFONT)SelectObject(hdc, self->m_fontPanelTitle);
            SetTextColor(hdc, RGB(27, 27, 27));
            DrawTextW(hdc, title, -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, oldFont);

            // 不再绘制 panel 外部框线 / 标题下方横线
            if (hwnd == self->m_containerPanel && !self->m_sectionRects.empty()) {
                HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(229, 229, 229));
                HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                for (const RECT& sr : self->m_sectionRects) {
                    Rectangle(hdc, sr.left, sr.top, rc.right - 1, sr.bottom - 1);
                }
                SelectObject(hdc, oldBrush);
                SelectObject(hdc, oldPen);
                DeleteObject(borderPen);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            if (dis->CtlID == IDC_OPEN_MODE_TOGGLE) {
                self->DrawOpenModeSwitch(dis->hDC, dis->rcItem, self->m_openModeState != 0);
                return TRUE;
            }
            if (dis->CtlID == IDC_MODE_SWITCH) {
                self->DrawShortcutModeSwitch(dis->hDC, dis->rcItem,
                                             g_settings.shortcutMode == ShortcutMode::Original);
                return TRUE;
            }
            if (dis->CtlID == IDC_DELETE_CONTAINER_BTN) {
                self->DrawDeleteButton(dis->hDC, dis->rcItem);
                return TRUE;
            }
            if (dis->CtlID >= IDC_SHORTCUT_LABEL_BASE && dis->CtlID < IDC_NOTE_BASE) {
                self->DrawShortcutLabel(dis->hDC, dis);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            HWND ctl = (HWND)lp;
            SetBkMode(hdc, TRANSPARENT);
            SetBkColor(hdc, RGB(255, 255, 255));
            int id = GetDlgCtrlID(ctl);
            SetTextColor(hdc, (id >= IDC_NOTE_BASE) ? RGB(97, 97, 97) : RGB(27, 27, 27));
            return (LRESULT)self->m_whiteBrush;
        }
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetBkColor((HDC)wp, RGB(255, 255, 255));
            return (LRESULT)self->m_whiteBrush;
        case WM_COMMAND:
            self->OnPanelCommand(hwnd, LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_HSCROLL:
            self->OnHScroll((HWND)lp, LOWORD(wp));
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 创建 panel ──────────────────────────────────────────────────────
HWND ManageTab::CreatePanel(const std::wstring& title) {
    HWND panel = CreateWindowExW(0, L"DeskStackManagePanel", title.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                 0, 0, 100, 100,
                                 m_right, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(panel, GWLP_USERDATA, (LONG_PTR)this);
    m_panels.push_back(panel);
    return panel;
}

void ManageTab::RefreshRightPane() {
    if (!m_right) return;

    if (m_previewIcon) { DestroyIcon(m_previewIcon); m_previewIcon = nullptr; }
    for (HWND p : m_panels)
        if (p && IsWindow(p)) DestroyWindow(p);
    m_panels.clear();
    m_panelHeights.clear();
    m_containerPanel = nullptr;
    m_sectionRects.clear();

    int settingsTab = (int)g_containers.size();
    if (m_selected < 0) m_selected = 0;
    if (m_selected > settingsTab) m_selected = settingsTab;

    if (m_selected == settingsTab)
        AddSettingsPanel();
    else if (m_selected >= 0 && m_selected < (int)g_containers.size())
        AddContainerPanel();
    else
        AddSettingsPanel();

    m_scrollPos = 0;
    LayoutPanels();
}

void ManageTab::OnSelectTab(int index) {
    if (index == m_selected) return;
    m_selected = index;
    InvalidateNav();
    RefreshRightPane();
}

// ── 控件辅助 ────────────────────────────────────────────────────────
HWND ManageTab::MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    return ctl;
}

HWND ManageTab::MakeButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id, bool enabled) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;
    if (!enabled) style |= WS_DISABLED;
    HWND ctl = CreateWindowExW(0, L"BUTTON", text, style,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    return ctl;
}

HWND ManageTab::MakeEdit(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, L"EDIT", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    return ctl;
}

HWND ManageTab::MakeDeleteButton(HWND parent, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, L"BUTTON", L"删除容器",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    return ctl;
}

HWND ManageTab::MakeCombo(HWND parent, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    return ctl;
}

HWND ManageTab::MakeSwitch(HWND parent, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, L"BUTTON", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | BS_NOTIFY,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    return ctl;
}

HWND ManageTab::MakeShortcutLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id) {
    HWND ctl = CreateWindowExW(0, L"STATIC", text,
                               WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    return ctl;
}

HWND ManageTab::MakeTrackbar(HWND parent, int x, int y, int w, int h, int id, int min, int max, int pos) {
    HWND ctl = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                               x, y, w, h, parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)m_fontText, TRUE);
    SendMessageW(ctl, TBM_SETRANGE, TRUE, MAKELPARAM(min, max));
    SendMessageW(ctl, TBM_SETPOS, TRUE, pos);
    return ctl;
}

HWND ManageTab::MakeIconPreview(HWND parent, int x, int y, int id) {
    HWND ctl = CreateWindowExW(0, L"STATIC", L"",
                               WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE | SS_REALSIZEIMAGE,
                               x, y, util::Scaled(56), util::Scaled(56),
                               parent, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    return ctl;
}

void ManageTab::LoadPreviewIcon() {
    if (m_previewIcon) { DestroyIcon(m_previewIcon); m_previewIcon = nullptr; }
    if (m_selected < 0 || m_selected >= (int)g_containers.size()) return;
    const auto& d = g_containers[m_selected]->Data();
    std::wstring src = d.iconPath.empty() ? ExePath() : d.iconPath;
    m_previewIcon = iconlib::IconForPath(src, 48);
}

// ── 容器 panel ──────────────────────────────────────────────────────
void ManageTab::AddContainerPanel() {
    int ci = m_selected;
    if (ci < 0 || ci >= (int)g_containers.size()) return;

    const auto& data = g_containers[ci]->Data();
    std::wstring title = L"容器设置";
    if (!data.name.empty()) title += L" — " + data.name;

    HWND panel = CreatePanel(title);
    m_containerPanel = panel;
    m_sectionRects.clear();
    m_openModeState = (data.openMode == OpenMode::Hover) ? 0 : 1;

    const int margin = util::Scaled(24);
    const int labelW = util::Scaled(110);
    const int ctrlX = margin + labelW;
    const int gap = util::Scaled(10);
    int y = util::Scaled(68);

    auto labelY = [&](int top, int h) { return top + (h - util::Scaled(24)) / 2; };

    // 显示名称
    {
        int secTop = y;
        int secH = util::Scaled(56);
        MakeLabel(panel, L"显示名称", margin, labelY(secTop, secH), labelW, util::Scaled(24), 0);
        MakeEdit(panel, data.name.c_str(), ctrlX, secTop + (secH - util::Scaled(24)) / 2,
                 util::Scaled(220), util::Scaled(24), IDC_NAME_EDIT);
        m_sectionRects.push_back({ 0, secTop, 0, secTop + secH });
        y = secTop + secH + gap;
    }

    // 打开方式
    {
        int secTop = y;
        int secH = util::Scaled(56);
        MakeLabel(panel, L"打开方式", margin, labelY(secTop, secH), labelW, util::Scaled(24), 0);
        MakeSwitch(panel, ctrlX, secTop + (secH - util::Scaled(32)) / 2,
                   util::Scaled(160), util::Scaled(32), IDC_OPEN_MODE_TOGGLE);
        m_sectionRects.push_back({ 0, secTop, 0, secTop + secH });
        y = secTop + secH + gap;
    }

    // 展开形式
    {
        int secTop = y;
        int secH = util::Scaled(56);
        MakeLabel(panel, L"展开形式", margin, labelY(secTop, secH), labelW, util::Scaled(24), 0);
        HWND combo = MakeCombo(panel, ctrlX, secTop + (secH - util::Scaled(24)) / 2,
                               util::Scaled(180), util::Scaled(120), IDC_STYLE_COMBO);
        const wchar_t* styles[] = { L"网格", L"单列", L"单行", L"扇形", L"环形" };
        for (const wchar_t* s : styles)
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s);
        SendMessageW(combo, CB_SETCURSEL, (WPARAM)(int)data.style, 0);
        m_sectionRects.push_back({ 0, secTop, 0, secTop + secH });
        y = secTop + secH + gap;
    }

    // 显示图标
    {
        int secTop = y;
        int secH = util::Scaled(76);
        MakeLabel(panel, L"显示图标", margin, labelY(secTop, secH), labelW, util::Scaled(24), 0);
        HWND icon = MakeIconPreview(panel, ctrlX, secTop + (secH - util::Scaled(56)) / 2, IDC_ICON_PREVIEW);
        LoadPreviewIcon();
        SendMessageW(icon, STM_SETICON, (WPARAM)m_previewIcon, 0);
        MakeButton(panel, L"更换图标", ctrlX + util::Scaled(70), secTop + (secH - util::Scaled(30)) / 2,
                   util::Scaled(90), util::Scaled(30), IDC_CHANGE_ICON_BTN);
        m_sectionRects.push_back({ 0, secTop, 0, secTop + secH });
        y = secTop + secH + gap;
    }

    // 内容列表（保持原样，不强制上下居中）
    {
        int secTop = y;
        MakeLabel(panel, L"内容列表", margin, secTop + util::Scaled(4), labelW, util::Scaled(24), 0);
        y = secTop + util::Scaled(30);

        for (size_t i = 0; i < data.shortcuts.size(); i++) {
            const ShortcutEntry& e = data.shortcuts[i];
            std::wstring text = e.name;
            if (!e.path.empty() && e.path != e.name)
                text += L"  —  " + e.path;
            MakeShortcutLabel(panel, text.c_str(), margin, y + util::Scaled(4),
                              util::Scaled(400), util::Scaled(24), IDC_SHORTCUT_LABEL_BASE + (int)i);
            MakeButton(panel, L"移除", margin + util::Scaled(400), y,
                       util::Scaled(64), util::Scaled(28), IDC_REMOVE_SHORTCUT_BASE + (int)i);
            y += util::Scaled(36);
        }

        MakeButton(panel, L"+ 增加", margin, y, util::Scaled(90), util::Scaled(30),
                   IDC_ADD_SHORTCUT_BTN);
        y += util::Scaled(44);

        // 删除容器
        MakeDeleteButton(panel, margin, y, util::Scaled(100), util::Scaled(30), IDC_DELETE_CONTAINER_BTN);
        MakeLabel(panel, L"删除当前容器，释放当前容器内所有快捷方式至桌面",
                  margin + util::Scaled(112), y + util::Scaled(5),
                  util::Scaled(320), util::Scaled(24), IDC_NOTE_BASE + 5);
        y += util::Scaled(44);

        m_sectionRects.push_back({ 0, secTop, 0, y });
    }

    m_panelHeights.push_back(y);
}

// ── 设置 panel ──────────────────────────────────────────────────────
void ManageTab::AddSettingsPanel() {
    HWND panel = CreatePanel(L"软件通用设置");
    m_containerPanel = panel;
    m_sectionRects.clear();

    const int margin = util::Scaled(24);
    const int labelW = util::Scaled(110);
    const int ctrlX = margin + labelW;
    const int gap = util::Scaled(10);
    int y = util::Scaled(68);

    // 原始文件：原件 / 引用 开关（带下拉说明）
    {
        int secTop = y;
        int secH = util::Scaled(64);
        MakeLabel(panel, L"原始文件", margin, secTop + (secH - util::Scaled(24)) / 2,
                  labelW, util::Scaled(24), 0);
        MakeSwitch(panel, ctrlX, secTop + (secH - util::Scaled(32)) / 2,
                   util::Scaled(160), util::Scaled(32), IDC_MODE_SWITCH);
        MakeButton(panel, m_originalExpanded ? L"v" : L">",
                   ctrlX + util::Scaled(300), secTop + (secH - util::Scaled(28)) / 2,
                   util::Scaled(28), util::Scaled(28), IDC_ORIGINAL_EXPAND);

        m_sectionRects.push_back({ 0, secTop, 0, secTop + secH });

        if (m_originalExpanded) {
            int descTop = secTop + secH;
            int descH = util::Scaled(56);
            MakeLabel(panel,
                      L"说明：只对快捷方式有效：原件-添加时原快捷方式移除，删除时原快捷方式返回源处；引用-添加/删除不会对原快捷方式产生影响",
                      margin, descTop + util::Scaled(6), util::Scaled(440), util::Scaled(44),
                      IDC_NOTE_BASE + 20);
            m_sectionRects.push_back({ 0, descTop, 0, descTop + descH });
            y = descTop + descH + gap;
        } else {
            y = secTop + secH + gap;
        }
    }

    struct SettingDef {
        const wchar_t* name;
        int sliderId;
        int valueId;
        int min;
        int max;
        int pos;
        std::wstring value;
        std::wstring note;
    };
    const SettingDef defs[4] = {
        { L"字符截断", IDC_SLIDER_MAX, IDC_VALUE_MAX, 0, 20, g_settings.maxChars,
          std::to_wstring(g_settings.maxChars),
          L"说明：快捷方式名称超过该字符数时截断显示" },
        { L"最大行数", IDC_SLIDER_LINES, IDC_VALUE_LINES, 1, 5, g_settings.maxLines,
          std::to_wstring(g_settings.maxLines),
          L"说明：快捷方式名称显示的最大行数" },
        { L"外径缩放", IDC_SLIDER_OUTER, IDC_VALUE_OUTER, 50, 200,
          (int)(g_settings.outerScale * 100.0),
          FormatDouble(g_settings.outerScale),
          L"说明：扇形和环形使用，大于1时外径外扩，小于1时外径内缩" },
        { L"内径缩放", IDC_SLIDER_INNER, IDC_VALUE_INNER, 50, 200,
          (int)(g_settings.innerScale * 100.0),
          FormatDouble(g_settings.innerScale),
          L"说明：扇形和环形使用，大于1时内径外扩，小于1时内径内缩" },
    };

    for (int i = 0; i < 4; i++) {
        const SettingDef& d = defs[i];
        int secTop = y;
        int secH = util::Scaled(64);

        MakeLabel(panel, d.name, margin, secTop + (secH - util::Scaled(24)) / 2,
                  labelW, util::Scaled(24), 0);
        MakeTrackbar(panel, ctrlX, secTop + (secH - util::Scaled(28)) / 2,
                     util::Scaled(240), util::Scaled(28), d.sliderId, d.min, d.max, d.pos);
        MakeLabel(panel, d.value.c_str(), ctrlX + util::Scaled(250), secTop + (secH - util::Scaled(24)) / 2,
                  util::Scaled(40), util::Scaled(24), d.valueId);
        MakeButton(panel, m_settingExpanded[i] ? L"v" : L">",
                   ctrlX + util::Scaled(300), secTop + (secH - util::Scaled(28)) / 2,
                   util::Scaled(28), util::Scaled(28), IDC_SETTING_EXPAND_BASE + i);

        m_sectionRects.push_back({ 0, secTop, 0, secTop + secH });

        if (m_settingExpanded[i]) {
            // 主面板底部与下拉说明面板顶部间距为 0
            int descTop = secTop + secH;
            int descH = util::Scaled(44);
            MakeLabel(panel, d.note.c_str(), margin, descTop + (descH - util::Scaled(24)) / 2,
                      util::Scaled(420), util::Scaled(24), IDC_NOTE_BASE + 10 + i);
            m_sectionRects.push_back({ 0, descTop, 0, descTop + descH });
            y = descTop + descH + gap;
        } else {
            y = secTop + secH + gap;
        }
    }

    m_panelHeights.push_back(y);
}

// ── panel 子控件响应 / 缩放 ─────────────────────────────────────────
void ManageTab::OnPanelCommand(HWND panel, int id, WORD code) {
    if (code == EN_CHANGE && id == IDC_NAME_EDIT) {
        OnNameChanged(panel);
        return;
    }
    if (code == CBN_SELCHANGE && id == IDC_STYLE_COMBO) {
        if (m_selected >= 0 && m_selected < (int)g_containers.size()) {
            HWND combo = GetDlgItem(panel, id);
            int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel <= 4) {
                g_containers[m_selected]->Data().style = (ExpandStyle)sel;
                Config::MarkDirty();
                Config::SaveNow();
                g_containers[m_selected]->CloseActivePanel();
            }
        }
        return;
    }
    if (code != BN_CLICKED) return;

    if (id == IDC_OPEN_MODE_TOGGLE) {
        m_openModeState = m_openModeState ? 0 : 1;
        if (m_selected >= 0 && m_selected < (int)g_containers.size())
            g_containers[m_selected]->Data().openMode =
                m_openModeState ? OpenMode::Click : OpenMode::Hover;
        Config::MarkDirty();
        Config::SaveNow();
        HWND btn = GetDlgItem(panel, id);
        if (btn) InvalidateRect(btn, nullptr, TRUE);
    } else if (id == IDC_MODE_SWITCH) {
        g_settings.shortcutMode = (g_settings.shortcutMode == ShortcutMode::Original)
            ? ShortcutMode::Reference : ShortcutMode::Original;
        Config::MarkDirty();
        Config::SaveNow();
        HWND btn = GetDlgItem(panel, id);
        if (btn) InvalidateRect(btn, nullptr, TRUE);
    } else if (id == IDC_CHANGE_ICON_BTN) {
        if (m_selected >= 0 && m_selected < (int)g_containers.size()) {
            std::wstring iconFile;
            if (PickIconFile(panel, iconFile) && !iconFile.empty()) {
                g_containers[m_selected]->SetIconPath(iconFile);
                Config::SaveNow();
                LoadNavIcons();
                RefreshRightPane();
                InvalidateNav();
            }
        }
    } else if (id == IDC_ADD_SHORTCUT_BTN) {
        if (m_selected >= 0 && m_selected < (int)g_containers.size()) {
            std::wstring picked;
            bool folder = false;
            if (PickPath(panel, picked, folder) && !picked.empty()) {
                g_containers[m_selected]->AddShortcut(picked);
                Config::SaveNow();
                RefreshRightPane();
            }
        }
    } else if (id >= IDC_REMOVE_SHORTCUT_BASE && id < IDC_SHORTCUT_LABEL_BASE) {
        if (m_selected >= 0 && m_selected < (int)g_containers.size()) {
            int idx = id - IDC_REMOVE_SHORTCUT_BASE;
            auto& sc = g_containers[m_selected]->Data().shortcuts;
            if (idx >= 0 && idx < (int)sc.size()) {
                RestoreShortcutToSource(sc[idx]);
                sc.erase(sc.begin() + idx);
                g_containers[m_selected]->Render();
                Config::MarkDirty();
                Config::SaveNow();
                RefreshRightPane();
            }
        }
    } else if (id == IDC_ORIGINAL_EXPAND) {
        m_originalExpanded = !m_originalExpanded;
        RefreshRightPane();
    } else if (id == IDC_DELETE_CONTAINER_BTN) {
        OnDeleteContainer();
    } else if (id >= IDC_SETTING_EXPAND_BASE && id < IDC_SETTING_EXPAND_BASE + 4) {
        OnToggleSetting(id - IDC_SETTING_EXPAND_BASE);
    }
}

void ManageTab::OnNameChanged(HWND panel) {
    if (m_selected < 0 || m_selected >= (int)g_containers.size()) return;
    HWND edit = GetDlgItem(panel, IDC_NAME_EDIT);
    if (!edit) return;
    wchar_t buf[256] = {};
    GetWindowTextW(edit, buf, 256);
    auto& container = g_containers[m_selected];
    container->Data().name = buf;
    if (container->Hwnd())
        SetWindowTextW(container->Hwnd(), buf);
    container->Render();
    Config::MarkDirty();
    Config::SaveNow();
    InvalidateNav();

    std::wstring newTitle = L"容器设置";
    if (buf[0]) { newTitle += L" — "; newTitle += buf; }
    SetWindowTextW(panel, newTitle.c_str());
    InvalidateRect(panel, nullptr, TRUE);
}

void ManageTab::OnAddContainer() {
    int n = (int)g_containers.size() + 1;
    ContainerData d;
    d.id = "c" + std::to_string(n);
    d.name = L"新容器";
    d.iconPath = ExePath();          // app.ico 作为默认容器图标
    d.style = ExpandStyle::Fan;
    d.openMode = OpenMode::Click;
    WORD cw = 0, ch = 0;
    desktop::GetCellSize(cw, ch);
    d.col = (int)(g_containers.size() % 7);
    d.row = 0;

    auto c = std::make_unique<ContainerWindow>(d);
    if (!c->Hwnd()) return;
    g_containers.push_back(std::move(c));

    Config::MarkDirty();
    Config::SaveNow();

    m_selected = (int)g_containers.size() - 1;
    LoadNavIcons();
    RefreshRightPane();
    InvalidateNav();
}

void ManageTab::OnDeleteContainer() {
    int ci = m_selected;
    if (ci < 0 || ci >= (int)g_containers.size()) return;

    // 仅原件模式的快捷方式返回原位置；引用/普通文件不影响原文件
    for (auto& e : g_containers[ci]->Data().shortcuts)
        RestoreShortcutToSource(e);

    g_containers.erase(g_containers.begin() + ci);
    Config::MarkDirty();
    Config::SaveNow();

    if (!g_containers.empty()) {
        if (m_selected >= (int)g_containers.size())
            m_selected = (int)g_containers.size() - 1;
    } else {
        m_selected = 0;
    }

    LoadNavIcons();
    RefreshRightPane();
    InvalidateNav();
}

void ManageTab::OnToggleSetting(int index) {
    if (index < 0 || index > 3) return;
    m_settingExpanded[index] = !m_settingExpanded[index];
    RefreshRightPane();
}

void ManageTab::OnHScroll(HWND track, WORD code) {
    if (!track || (code != TB_THUMBTRACK && code != TB_THUMBPOSITION && code != TB_ENDTRACK &&
                   code != TB_LINEUP && code != TB_LINEDOWN && code != TB_PAGEUP && code != TB_PAGEDOWN &&
                   code != TB_TOP && code != TB_BOTTOM))
        return;

    int id = GetDlgCtrlID(track);
    int pos = (int)SendMessageW(track, TBM_GETPOS, 0, 0);
    HWND parent = GetParent(track);
    wchar_t buf[32] = {};

    if (id == IDC_SLIDER_MAX || id == IDC_SLIDER_LINES) {
        std::swprintf(buf, 32, L"%d", pos);
        int valId = (id == IDC_SLIDER_MAX) ? IDC_VALUE_MAX : IDC_VALUE_LINES;
        HWND val = GetDlgItem(parent, valId);
        if (val) SetWindowTextW(val, buf);

        if (id == IDC_SLIDER_MAX)
            g_settings.maxChars = pos;
        else
            g_settings.maxLines = pos;
    } else if (id == IDC_SLIDER_OUTER || id == IDC_SLIDER_INNER) {
        std::wstring text = FormatDouble(pos / 100.0);
        int valId = (id == IDC_SLIDER_OUTER) ? IDC_VALUE_OUTER : IDC_VALUE_INNER;
        HWND val = GetDlgItem(parent, valId);
        if (val) SetWindowTextW(val, text.c_str());

        if (id == IDC_SLIDER_OUTER)
            g_settings.outerScale = pos / 100.0;
        else
            g_settings.innerScale = pos / 100.0;
    }

    Config::MarkDirty();
    Config::SaveNow();
}

void ManageTab::ResizePanelChildren(HWND panel, int width) {
    if (!panel || !IsWindow(panel)) return;

    const int margin = util::Scaled(24);
    const int removeW = util::Scaled(64);
    HWND child = GetWindow(panel, GW_CHILD);
    while (child) {
        int id = GetDlgCtrlID(child);
        RECT rc;
        GetWindowRect(child, &rc);
        MapWindowPoints(nullptr, panel, (LPPOINT)&rc, 2);
        int h = rc.bottom - rc.top;

        if (id == IDC_CHANGE_ICON_BTN) {
            SetWindowPos(child, nullptr, width - margin - util::Scaled(90), rc.top,
                         util::Scaled(90), h, SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (id >= IDC_SETTING_EXPAND_BASE && id <= IDC_ORIGINAL_EXPAND) {
            SetWindowPos(child, nullptr, width - margin - util::Scaled(28), rc.top,
                         util::Scaled(28), h, SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (id >= IDC_REMOVE_SHORTCUT_BASE && id < IDC_SHORTCUT_LABEL_BASE) {
            SetWindowPos(child, nullptr, width - margin - removeW, rc.top,
                         removeW, h, SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (id >= IDC_SHORTCUT_LABEL_BASE && id < IDC_NOTE_BASE) {
            int labelRight = width - margin - removeW - util::Scaled(8);
            int newW = labelRight - rc.left;
            if (newW < util::Scaled(80)) newW = util::Scaled(80);
            SetWindowPos(child, nullptr, rc.left, rc.top, newW, h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }

        child = GetWindow(child, GW_HWNDNEXT);
    }
}
