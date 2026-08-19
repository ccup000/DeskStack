#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include "container.h"
#include "panel.h"
#include "layered.h"
#include "iconlib.h"
#include "desktop.h"
#include "appstate.h"
#include "uinp.h"
#include <objidl.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>

using namespace Gdiplus;

namespace {

const WCHAR* kFont = L"Microsoft YaHei UI";

std::wstring BaseName(const std::wstring& path) {
    wchar_t tmp[MAX_PATH] = {};
    wcscpy_s(tmp, path.c_str());
    LPWSTR name = PathFindFileNameW(tmp);
    return name ? name : path;
}

std::wstring NameNoExt(const std::wstring& path) {
    std::wstring base = BaseName(path);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        return base.substr(0, dot);
    return base;
}

// 把网格索引收敛到“宿主客户区在工作区内可见”的范围内。
// 桌面图标列表的客户区原点/尺寸可能与工作区不完全重合，因此把宿主的 (0,0)
// 换算到屏幕坐标后再计算可见的 col/row 区间，避免容器被保存到屏幕外。
void ClampGridToWorkArea(HWND host, int cw, int ch, int& col, int& row) {
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    POINT origin{ 0, 0 };
    MapWindowPoints(host, nullptr, &origin, 1);

    int colMin = (int)std::ceil((double)(wa.left - origin.x) / cw);
    int rowMin = (int)std::ceil((double)(wa.top - origin.y) / ch);
    int colMax = (int)std::floor((double)(wa.right - cw - origin.x) / cw);
    int rowMax = (int)std::floor((double)(wa.bottom - ch - origin.y) / ch);
    if (colMin < 0) colMin = 0;
    if (rowMin < 0) rowMin = 0;
    if (colMax < colMin) colMax = colMin;
    if (rowMax < rowMin) rowMax = rowMin;

    if (col < colMin) col = colMin;
    if (col > colMax) col = colMax;
    if (row < rowMin) row = rowMin;
    if (row > rowMax) row = rowMax;
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

// 判断位图是否为空/全透明；全透明时不能作为可见图标，应回退到默认图标。
bool IsBitmapVisible(Bitmap* bmp) {
    if (!bmp) return false;
    int w = bmp->GetWidth(), h = bmp->GetHeight();
    if (w <= 0 || h <= 0) return false;
    // 采样一部分像素即可，不必逐像素扫描，节省绘制时间。
    int step = std::max(1, (w * h) / 400);
    int checked = 0, nonzero = 0;
    for (int y = 0; y < h; y += 4) {
        for (int x = 0; x < w; x += 4) {
            Color c;
            if (bmp->GetPixel(x, y, &c) == Ok && c.GetA() > 0) {
                if (++nonzero >= 8) return true;
            }
            if (++checked >= 400) break;
        }
        if (checked >= 400) break;
    }
    return nonzero >= 8;
}

// 图标解析失败时绘制可见占位块
void DrawFallbackIcon(Graphics& g, const WCHAR* text, int x, int y, int w, int h) {
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

// ── OLE 拖放目标（接收从桌面/资源管理器拖入的 .lnk / .exe / 文件夹）──
static UINT s_cfShellIdList = 0;

class ContainerDropTarget : public IDropTarget {
    LONG m_ref = 1;
    HWND m_hwnd = nullptr;
    bool m_over = false;
public:
    explicit ContainerDropTarget(HWND h) : m_hwnd(h) {}

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)()  { return InterlockedIncrement(&m_ref); }
    STDMETHOD_(ULONG, Release)() {
        LONG c = InterlockedDecrement(&m_ref);
        if (c == 0) delete this;
        return c;
    }
    STDMETHOD(DragEnter)(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) {
        if (!pDataObj) { m_over = false; *pdwEffect = DROPEFFECT_NONE; return S_OK; }
        FORMATETC st = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        FORMATETC stShell = { (CLIPFORMAT)s_cfShellIdList, nullptr,
                              DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        m_over = (pDataObj->QueryGetData(&st) == S_OK) ||
                 (s_cfShellIdList &&
                  pDataObj->QueryGetData(&stShell) == S_OK);
        *pdwEffect = m_over ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        if (m_over) {
            auto* self = (ContainerWindow*)GetWindowLongPtrW(m_hwnd, GWLP_USERDATA);
            if (self) self->SetDragOver(true);
        }
        return S_OK;
    }
    STDMETHOD(DragOver)(DWORD, POINTL, DWORD* pdwEffect) {
        *pdwEffect = m_over ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHOD(DragLeave)() {
        m_over = false;
        auto* self = (ContainerWindow*)GetWindowLongPtrW(m_hwnd, GWLP_USERDATA);
        if (self) self->SetDragOver(false);
        return S_OK;
    }
    STDMETHOD(Drop)(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) {
        m_over = false;
        auto* self = (ContainerWindow*)GetWindowLongPtrW(m_hwnd, GWLP_USERDATA);
        if (!self || !pDataObj) { *pdwEffect = DROPEFFECT_NONE; return S_OK; }
        self->SetDragOver(false);

        // 优先读 CF_HDROP（文件系统路径，覆盖 .lnk/.exe/文件夹）
        FORMATETC ft = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm = {};
        if (SUCCEEDED(pDataObj->GetData(&ft, &sm)) && sm.hGlobal) {
            HDROP hd = (HDROP)GlobalLock(sm.hGlobal);
            UINT n = hd ? DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0) : 0;
            for (UINT i = 0; i < n; i++) {
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW(hd, i, path, MAX_PATH) && path[0])
                    self->AddShortcut(path);
            }
            if (hd) GlobalUnlock(sm.hGlobal);
            ReleaseStgMedium(&sm);
            *pdwEffect = DROPEFFECT_MOVE;
            return S_OK;
        }
        // 兜底：shell namespace 对象（CFSTR_SHELLIDLIST）
        if (s_cfShellIdList) {
            FORMATETC ft2 = { (CLIPFORMAT)s_cfShellIdList, nullptr,
                              DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM sm2 = {};
            if (SUCCEEDED(pDataObj->GetData(&ft2, &sm2))) {
                IShellItemArray* items = nullptr;
                if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(pDataObj, IID_PPV_ARGS(&items))) && items) {
                    DWORD n = 0; items->GetCount(&n);
                    for (DWORD i = 0; i < n; i++) {
                        IShellItem* it = nullptr;
                        if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                            LPWSTR pw = nullptr;
                            if (SUCCEEDED(it->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &pw)) && pw) {
                                self->AddShortcut(pw);
                                CoTaskMemFree(pw);
                            }
                            it->Release();
                        }
                    }
                    items->Release();
                }
                ReleaseStgMedium(&sm2);
                *pdwEffect = DROPEFFECT_MOVE;
                return S_OK;
            }
        }
        *pdwEffect = DROPEFFECT_NONE;
        return S_OK;
    }
};

// 手动“添加快捷方式”：先选文件夹，再选文件
// 构建带 NUL 分隔的打开文件过滤器（运行时拼接，避免源内嵌 NUL）
std::wstring MakeFileFilter() {
    std::wstring f;
    f += L"快捷方式 (*.lnk)"; f.push_back(L'\0'); f += L"*.lnk";
    f.push_back(L'\0'); f += L"程序 (*.exe)"; f.push_back(L'\0'); f += L"*.exe";
    f.push_back(L'\0'); f += L"所有文件 (*.*)"; f.push_back(L'\0'); f += L"*.*";
    f.push_back(L'\0');
    return f;
}

// 手动添加：选择文件 / 快捷方式（.lnk/.exe/任意文件）
bool PickFile(HWND owner, std::wstring& out) {
    OPENFILENAMEW ofn = { sizeof(ofn) };
    wchar_t file[MAX_PATH] = {};
    std::wstring filter = MakeFileFilter();
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择快捷方式 / 程序";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn) && file[0]) { out = file; return true; }
    return false;
}

// 手动添加：选择文件夹
bool PickFolder(HWND owner, std::wstring& out) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择要收纳的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t buf[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, buf) && buf[0]) out = buf;
    CoTaskMemFree(pidl);
    return !out.empty();
}

// 构建带 NUL 分隔的“选择容器图标”文件过滤器
std::wstring MakeIconFilter() {
    std::wstring f;
    f += L"图标文件 (*.ico)"; f.push_back(L'\0'); f += L"*.ico";
    f.push_back(L'\0'); f += L"程序 (*.exe)"; f.push_back(L'\0'); f += L"*.exe";
    f.push_back(L'\0'); f += L"所有文件 (*.*)"; f.push_back(L'\0'); f += L"*.*";
    f.push_back(L'\0');
    return f;
}

// 手动设置容器图标：选择 .ico 或 .exe（从 exe 读取内嵌图标）
bool PickIconFile(HWND owner, std::wstring& out) {
    OPENFILENAMEW ofn = { sizeof(ofn) };
    wchar_t file[MAX_PATH] = {};
    std::wstring filter = MakeIconFilter();
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择容器图标（.ico 或 .exe）";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn) && file[0]) { out = file; return true; }
    return false;
}

} // namespace

void ContainerWindow::RegisterClass(HINSTANCE hInst) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = ContainerWindow::WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"DeskStackContainer";
    RegisterClassExW(&wc);
}

ContainerWindow::ContainerWindow(const ContainerData& data) : m_data(data) {
    HWND host = desktop::FindDesktopListView();
    int w = util::Scaled(113), h = util::Scaled(103);

    // 计算初始位置与尺寸（网格索引 × 单元格）。
    // 容器窗口尺寸取桌面网格单元格大小，这样图标/文字在单元格内居中，
    // 与桌面“图标与网格对齐”时的其他图标保持一致。
    POINT pt{ 0, 0 };
    if (host) {
        WORD cw = 0, ch = 0;
        if (desktop::GetCellSize(cw, ch) && cw > 0 && ch > 0) {
            ClampGridToWorkArea(host, (int)cw, (int)ch, m_data.col, m_data.row);
            pt.x = m_data.col * (int)cw;
            pt.y = m_data.row * (int)ch;
            w = (int)cw;
            h = (int)ch;
        }
    }

    m_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                             L"DeskStackContainer", m_data.name.c_str(),
                             host ? WS_CHILD : WS_POPUP,
                             pt.x, pt.y, w, h,
                             host, nullptr, GetModuleHandleW(nullptr), this);
    if (!m_hwnd) return;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    if (host)
        SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // OLE 拖放接收：支持 CF_HDROP（.lnk/.exe/文件夹）与 shell 命名空间对象
    if (!s_cfShellIdList)
        s_cfShellIdList = (UINT)RegisterClipboardFormatW(L"Shell IDList Array");
    auto* dt = new ContainerDropTarget(m_hwnd);
    if (SUCCEEDED(RegisterDragDrop(m_hwnd, dt)))
        dt->Release();
    else {
        delete dt;
        DragAcceptFiles(m_hwnd, TRUE);   // 兜底：仅文件系统路径
    }
    ShowWindow(m_hwnd, host ? SW_SHOW : SW_HIDE);
    ReapplyPosition();
    Render();
}

ContainerWindow::~ContainerWindow() {
    if (m_panel) { delete m_panel; m_panel = nullptr; }
    if (m_hwnd && IsWindow(m_hwnd)) {
        RevokeDragDrop(m_hwnd);
        DragAcceptFiles(m_hwnd, FALSE);
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
}

void ContainerWindow::ReapplyPosition() {
    if (!m_hwnd) return;
    if (m_dragging) return;          // 拖拽进行中不重定位，避免与用户打架
    HWND host = desktop::FindDesktopListView();
    if (!host) return;
    WORD cw = 0, ch = 0;
    if (!desktop::GetCellSize(cw, ch) || cw <= 0 || ch <= 0) return;
    // 若保存的网格索引在工作区之外（例如旧配置 row 过大），先拉回可见范围
    ClampGridToWorkArea(host, (int)cw, (int)ch, m_data.col, m_data.row);
    int x = m_data.col * (int)cw;
    int y = m_data.row * (int)ch;
    // 同时更新为桌面单元格尺寸，保证网格对齐（不随 DPI/图标大小变化而偏移）
    SetWindowPos(m_hwnd, HWND_TOP, x, y, (int)cw, (int)ch,
                 SWP_NOACTIVATE);
    // 给容器设置矩形命中区域，避免透明区域导致 WindowFromPoint/鼠标点击穿透。
    SetWindowRgn(m_hwnd, CreateRectRgn(0, 0, (int)cw, (int)ch), TRUE);
    // 确保宿主可用后容器可见（解决：启动时宿主未就绪导致一直隐藏）
    if (!IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_SHOW);
    Render();
}

void ContainerWindow::SetGridIndex(int col, int row) {
    m_data.col = col;
    m_data.row = row;
}

void ContainerWindow::SetDragOver(bool over) {
    if (m_dragOver == over) return;
    m_dragOver = over;
    Render();
}

void ContainerWindow::SetIconPath(const std::wstring& path) {
    m_data.iconPath = path;
    Render();
    Config::MarkDirty();
}

bool ContainerWindow::HitDraggable(POINT) const {
    // 整个容器块都可拖动
    return true;
}

void ContainerWindow::Render() {
    if (!m_hwnd) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT wr; GetWindowRect(m_hwnd, &wr);
    layered::Present(m_hwnd, w, h, wr.left, wr.top,
        [&](Graphics& g, int ww, int hh) {
            g.Clear(Color(0, 0, 0, 0));
            int icon = desktop::DesktopIconSize();   // 按桌面实际图标尺寸绘制，与系统桌面图标一致
            int x = (ww - icon) / 2, y = util::Scaled(6);
            Bitmap* iconBmp = iconlib::IconBitmapForPath(m_data.iconPath, icon);
            if (iconBmp && IsBitmapVisible(iconBmp)) {
                DrawBitmapIcon(g, iconBmp, x, y, icon, icon);
                delete iconBmp;
            } else {
                if (iconBmp) delete iconBmp;
                HICON ic = m_data.iconPath.empty()
                    ? iconlib::DefaultIcon(icon)
                    : iconlib::IconForPath(m_data.iconPath, icon);
                if (ic) {
                    DrawHIcon(g, ic, x, y, icon, icon);
                    DestroyIcon(ic);
                } else {
                    DrawFallbackIcon(g, m_data.name.c_str(), x, y, icon, icon);
                }
            }
            // 标签：白色 + 黑色阴影，字号与桌面图标一致（12px）。
            // 文本限制在容器窗口内（桌面网格单元格宽度），超长省略，避免越界。
            Font fn(kFont, (REAL)util::Scaled(12), FontStyleRegular, UnitPixel);
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            sf.SetTrimming(StringTrimmingEllipsisCharacter);
            int cy = y + icon + util::Scaled(8);
            int labelW = ww - util::Scaled(4);
            if (labelW < util::Scaled(20)) labelW = util::Scaled(20);
            int labelH = util::Scaled(18);
            RectF rcTxt((float)((ww - labelW) / 2), (float)(cy - labelH / 2),
                        (float)labelW, (float)labelH);
            SolidBrush sh(Color(180, 0, 0, 0));
            RectF rcSh(rcTxt); rcSh.X += 1; rcSh.Y += 1;
            g.DrawString(m_data.name.c_str(), -1, &fn, rcSh, &sf, &sh);
            SolidBrush wh(Color(255, 255, 255, 255));
            g.DrawString(m_data.name.c_str(), -1, &fn, rcTxt, &sf, &wh);

            // 拖放悬停高亮：容器外框淡蓝色圆角
            if (m_dragOver) {
                Pen pen(Color(160, 60, 140, 255), 3.0f);
                GraphicsPath gp;
                int r = 14;
                gp.AddArc(2, 2, r, r, 180, 90);
                gp.AddArc(ww - r - 2, 2, r, r, 270, 90);
                gp.AddArc(ww - r - 2, hh - r - 2, r, r, 0, 90);
                gp.AddArc(2, hh - r - 2, r, r, 90, 90);
                gp.CloseFigure();
                g.DrawPath(&pen, &gp);
            }
        });
}

void ContainerWindow::OpenPanel() {
    // 先关掉已有的其他面板
    for (auto& c : g_containers)
        if (c.get() != this && c->m_panel) { delete c->m_panel; c->m_panel = nullptr; }
    if (m_panel) { delete m_panel; m_panel = nullptr; }
    if (!m_hwnd) return;
    m_panel = new PanelWindow(this, m_data);
}

void ContainerWindow::CloseActivePanel() {
    if (m_panel) { delete m_panel; m_panel = nullptr; }
}


void ContainerWindow::AddShortcut(const std::wstring& srcPath) {
    std::wstring path = srcPath;
    std::wstring name = NameNoExt(path);
    std::string type = util::GuessType(path);

    // .lnk 从桌面拖入 → 收纳：先复制到应用库目录，再从桌面移除
    if (type == "lnk") {
        wchar_t appdata[MAX_PATH] = {};
        SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
        std::wstring lib = std::wstring(appdata) + L"/DeskStack/library";
        CreateDirectoryW(lib.c_str(), nullptr);
        std::wstring dst = lib + L"\\" + BaseName(path);
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyFileW(path.c_str(), dst.c_str(), FALSE);
        // 若源位于桌面则移除
        std::wstring desk = desktop::DesktopFolder() + L"\\";
        if (path.compare(0, desk.size(), desk) == 0)
            DeleteFileW(path.c_str());
        path = dst;
    }
    m_data.shortcuts.push_back({ name, path, type });
    Render();
    Config::MarkDirty();
}

LRESULT CALLBACK ContainerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ContainerWindow* self = (ContainerWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_NCHITTEST:
            // 分层窗口透明区域也视为可点击，避免容器空白处被桌面穿透。
            return HTCLIENT;
        case WM_DROPFILES: {
            if (!self) break;
            HDROP drop = (HDROP)wp;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; i++) {
                wchar_t path[MAX_PATH] = {};
                DragQueryFileW(drop, i, path, MAX_PATH);
                if (path[0]) self->AddShortcut(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!self) break;
            self->m_downClient = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            self->m_pressed = true;
            self->m_dragging = false;
            RECT rc; GetWindowRect(hwnd, &rc);
            self->m_downStartParent = { rc.left, rc.top };
            self->m_offsetX = self->m_downClient.x;
            self->m_offsetY = self->m_downClient.y;
            SetCapture(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!self || !self->m_pressed) break;
            POINT cur; GetCursorPos(&cur);
            if (!self->m_dragging) {
                int tox = abs(cur.x - (self->m_downStartParent.x + self->m_offsetX));
                int toy = abs(cur.y - (self->m_downStartParent.y + self->m_offsetY));
                if (tox < 4 && toy < 4) break;   // 还没超过拖动阈值
                self->m_dragging = true;
            }
            HWND host = desktop::FindDesktopListView();
            if (!host) break;
            POINT p = desktop::ScreenToClientOf(host, cur);
            int nx = p.x - self->m_offsetX;
            int ny = p.y - self->m_offsetY;
            SetWindowPos(hwnd, HWND_TOP, nx, ny, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (!self) break;
            ReleaseCapture();
            bool wasDrag = self->m_dragging;
            self->m_pressed = false;
            self->m_dragging = false;
            if (wasDrag) {
                // 吸附到桌面网格
                HWND host = desktop::FindDesktopListView();
                WORD cw = 0, ch = 0;
                if (host && desktop::GetCellSize(cw, ch) && cw > 0 && ch > 0) {
                    RECT rc; GetWindowRect(hwnd, &rc);
                    POINT p = desktop::ScreenToClientOf(host, { rc.left, rc.top });
                    int col = (int)((double)p.x / cw + 0.499);
                    int row = (int)((double)p.y / ch + 0.499);
                    // 拖拽释放也收敛到工作区可见网格内，避免保存/显示到屏幕外
                    ClampGridToWorkArea(host, (int)cw, (int)ch, col, row);
                    SetWindowPos(hwnd, HWND_TOP, col * (int)cw, row * (int)ch,
                                 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                    self->SetGridIndex(col, row);
                    Config::MarkDirty();
                }
            } else {
                self->OpenPanel();   // 单击 → 弹出面板
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            if (!self) break;
            POINT cur; GetCursorPos(&cur);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1001, L"修改容器名称");
            HMENU styles = CreatePopupMenu();
            const wchar_t* names[] = { L"网格", L"单列", L"单行", L"扇形", L"环形" };
            int curStyle = (int)self->m_data.style;
            for (int i = 0; i < 5; i++)
                AppendMenuW(styles, MF_STRING | (i == curStyle ? MF_CHECKED : 0),
                            2000 + i, names[i]);
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)styles, L"展开样式");
            AppendMenuW(menu, MF_STRING, 1004, L"添加文件 / 快捷方式…");
            AppendMenuW(menu, MF_STRING, 1005, L"添加文件夹…");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            {
                bool hasIcon = !self->m_data.iconPath.empty();
                AppendMenuW(menu, MF_STRING, 1006, L"设置容器图标…");
                AppendMenuW(menu, MF_STRING | (hasIcon ? 0 : MF_GRAYED), 1007, L"恢复默认图标");
            }
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 1002, L"删除本容器");
            AppendMenuW(menu, MF_STRING, 1003, L"打开管理界面");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                     cur.x, cur.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd >= 2000 && cmd <= 2004) {
                self->m_data.style = (ExpandStyle)(cmd - 2000);
                Config::MarkDirty();
            } else if (cmd == 1001) {
                std::wstring res;
                if (ShowInputBox(hwnd, L"修改容器名称", L"请输入容器名称：",
                                 self->m_data.name, res) && !res.empty()) {
                    self->m_data.name = res;
                    SetWindowTextW(hwnd, res.c_str());
                    self->Render();
                    Config::MarkDirty();
                }
            } else if (cmd == 1002) {
                // 删除：把快捷方式放回桌面，再移除容器
                for (auto& e : self->m_data.shortcuts) {
                    std::wstring target = e.path;
                    if (e.type == "lnk") {
                        std::wstring t = desktop::LnkTarget(e.path);
                        if (!t.empty()) target = t;
                    }
                    desktop::CreateDesktopShortcut(e.name, target);
                }
                if (g_owner) PostMessageW(g_owner, WM_CONTAINER_DELETE, (WPARAM)hwnd, 0);
            } else if (cmd == 1003) {
                if (g_owner) PostMessageW(g_owner, WM_OPEN_MANAGER, 0, 0);
            } else if (cmd == 1004 || cmd == 1005) {
                std::wstring picked;
                bool ok = (cmd == 1004) ? PickFile(hwnd, picked) : PickFolder(hwnd, picked);
                if (ok && !picked.empty())
                    self->AddShortcut(picked);
            } else if (cmd == 1006) {
                std::wstring iconFile;
                if (PickIconFile(hwnd, iconFile) && !iconFile.empty())
                    self->SetIconPath(iconFile);
            } else if (cmd == 1007) {
                self->SetIconPath(L"");
            }
            return 0;
        }
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
