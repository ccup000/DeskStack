#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include "manager.h"
#include "container.h"
#include "types.h"
#include "config.h"
#include "desktop.h"
#include "iconlib.h"
#include "appstate.h"
#include "resource.h"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

namespace {

// 控件 ID
enum {
    ID_LIST_CONTAINERS = 200,
    ID_LIST_SHORTCUTS  = 201,
    ID_COMBO_STYLE     = 202,
    ID_EDIT_NAME       = 203,
    ID_ICON_PREVIEW    = 204,
    ID_BTN_NEW            = 300,
    ID_BTN_CHANGE_ICON    = 301,
    ID_BTN_DELETE_CTX     = 302,
    ID_BTN_STYLE          = 303,
    ID_BTN_ADD_SHORTCUT   = 304,
    ID_BTN_DEL_SC         = 305,
    ID_BTN_CLOSE          = 306,
};

HWND hListCtx=nullptr, hListSc=nullptr, hCombo=nullptr, hEditName=nullptr, hIconPreview=nullptr;
HICON g_previewIcon = nullptr;
int g_renameIdx = -1;    // 正在双击重命名的容器下标，-1 表示不在编辑

int SelectedContainerIndex() {
    int i = (int)SendMessageW(hListCtx, LB_GETCURSEL, 0, 0);
    if (i == LB_ERR) return -1;
    return i;
}

std::wstring ExePathW() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

void RefreshContainers() {
    SendMessageW(hListCtx, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_containers.size(); i++)
        SendMessageW(hListCtx, LB_ADDSTRING, 0, (LPARAM)g_containers[i]->Data().name.c_str());
}

void RefreshShortcuts() {
    SendMessageW(hListSc, LB_RESETCONTENT, 0, 0);
    int ci = SelectedContainerIndex();
    if (ci < 0 || (size_t)ci >= g_containers.size()) return;
    for (auto& e : g_containers[ci]->Data().shortcuts)
        SendMessageW(hListSc, LB_ADDSTRING, 0, (LPARAM)e.name.c_str());
}

void SyncStyleCombo() {
    int ci = SelectedContainerIndex();
    SendMessageW(hCombo, CB_SETCURSEL, 0, 0);
    if (ci < 0 || (size_t)ci >= g_containers.size()) return;
    SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)g_containers[ci]->Data().style, 0);
}

void RefreshIconPreview() {
    if (g_previewIcon) { DestroyIcon(g_previewIcon); g_previewIcon = nullptr; }
    int ci = SelectedContainerIndex();
    if (ci >= 0 && (size_t)ci < g_containers.size()) {
        const auto& d = g_containers[ci]->Data();
        std::wstring src = d.iconPath.empty() ? ExePathW() : d.iconPath;
        g_previewIcon = iconlib::IconForPath(src, 64);
    }
    if (hIconPreview) SendMessageW(hIconPreview, STM_SETICON, (WPARAM)g_previewIcon, 0);
}

void UpdateButtons(HWND h) {
    int ci = SelectedContainerIndex();
    int si = (int)SendMessageW(hListSc, LB_GETCURSEL, 0, 0);
    if (si == LB_ERR) si = -1;
    bool hasCtx = (ci >= 0 && (size_t)ci < g_containers.size());
    EnableWindow(GetDlgItem(h, ID_BTN_CHANGE_ICON), hasCtx ? TRUE : FALSE);
    EnableWindow(GetDlgItem(h, ID_BTN_DELETE_CTX), hasCtx ? TRUE : FALSE);
    EnableWindow(GetDlgItem(h, ID_BTN_STYLE), hasCtx ? TRUE : FALSE);
    EnableWindow(GetDlgItem(h, ID_COMBO_STYLE), hasCtx ? TRUE : FALSE);
    EnableWindow(GetDlgItem(h, ID_BTN_ADD_SHORTCUT), hasCtx ? TRUE : FALSE);
    EnableWindow(GetDlgItem(h, ID_BTN_DEL_SC), (hasCtx && si >= 0) ? TRUE : FALSE);
}

void CreateContainer() {
    int n = (int)g_containers.size() + 1;
    ContainerData d;
    d.id  = "c" + std::to_string(n);
    d.name = L"新容器" + std::to_wstring(n);
    d.iconPath = ExePathW();
    WORD cw = 0, ch = 0; desktop::GetCellSize(cw, ch);
    d.col = (int)(g_containers.size() % 7); d.row = 0;
    auto c = std::make_unique<ContainerWindow>(d);
    if (!c->Hwnd()) return;
    g_containers.push_back(std::move(c));
    Config::MarkDirty();
}

// 提交重命名：读取编辑框 → 更新容器 → 刷新列表并重新选中被改名的项
void CommitRename(HWND h) {
    if (g_renameIdx < 0 || (size_t)g_renameIdx >= g_containers.size()) { g_renameIdx = -1; return; }
    wchar_t buf[256] = {};
    GetWindowTextW(hEditName, buf, 256);
    int keep = g_renameIdx;
    if (buf[0]) {
        auto& c = g_containers[keep];
        c->Data().name = buf;
        SetWindowTextW(c->Hwnd(), buf);
        c->Render();
        Config::MarkDirty();
    }
    g_renameIdx = -1;
    RefreshContainers();
    if ((size_t)keep < g_containers.size()) {
        SendMessageW(hListCtx, LB_SETCURSEL, keep, 0);
        RefreshShortcuts(); SyncStyleCombo(); RefreshIconPreview(); UpdateButtons(h);
    }
}

std::wstring MakeFilter(const wchar_t* a, const wchar_t* pa, const wchar_t* b, const wchar_t* pb) {
    std::wstring f;
    f += a; f += wchar_t(0); f += pa; f += wchar_t(0);
    f += b; f += wchar_t(0); f += pb; f += wchar_t(0);
    return f;
}

bool PickIconFile(HWND owner, std::wstring& out) {
    std::wstring filter = MakeFilter(L"图标文件 (*.ico)", L"*.ico", L"程序 (*.exe)", L"*.exe");
    OPENFILENAMEW ofn = { sizeof(ofn) };
    wchar_t file[MAX_PATH] = {};
    ofn.hwndOwner = owner; ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.lpstrTitle = L"选择容器图标";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn) && file[0]) { out = file; return true; }
    return false;
}

bool PickPath(HWND owner, std::wstring& out, bool& isFolder) {
    BROWSEINFOW bi = {}; bi.hwndOwner = owner;
    bi.lpszTitle = L"选择要收纳的文件夹（取消则选择文件）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t buf[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, buf)) { out = buf; isFolder = true; }
        CoTaskMemFree(pidl); return isFolder;
    }
    std::wstring filter = MakeFilter(L"所有文件 (*.*)", L"*.*", L"快捷方式 (*.lnk)", L"*.lnk");
    OPENFILENAMEW ofn = { sizeof(ofn) };
    wchar_t file[MAX_PATH] = {};
    ofn.hwndOwner = owner; ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) { out = file; isFolder = false; return true; }
    return false;
}

} // namespace

void OpenManagerWindow(HWND owner) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l)->LRESULT {
            switch (m) {
                case WM_CREATE: {
                    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                    auto ctl = [&](const wchar_t* cls, const wchar_t* txt, DWORD style,
                               int x, int y, int cx, int cy, int id) -> HWND {
                        HWND hc = CreateWindowExW(0, cls, txt,
                            WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, h,
                            (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
                        SendMessageW(hc, WM_SETFONT, (WPARAM)font, TRUE);
                        return hc;
                    };
                    ctl(L"BUTTON", L"容器", BS_GROUPBOX, 10, 10, 340, 258, 0);
                    hListCtx = ctl(L"LISTBOX", L"", WS_BORDER|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
                                  20, 30, 198, 178, ID_LIST_CONTAINERS);
                    hEditName = ctl(L"EDIT", L"", WS_BORDER|ES_AUTOHSCROLL|WS_TABSTOP,
                                  20, 216, 198, 24, ID_EDIT_NAME);
                    const int bx=228, bw=108, bh=24;
                    ctl(L"BUTTON", L"新建容器", BS_PUSHBUTTON, bx, 30, bw, bh, ID_BTN_NEW);
                    hCombo = ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                                  bx, 58, bw, 120, ID_COMBO_STYLE);
                    { const wchar_t* st[] = { L"网格", L"单列", L"单行", L"扇形", L"环形" };
                      for (auto* s : st) SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)s); }
                    ctl(L"BUTTON", L"更换图标", BS_PUSHBUTTON, bx, 88, bw, bh, ID_BTN_CHANGE_ICON);
                    // 更换图标 与 删除容器 之间有明显间距（大 y 间隔）
                    ctl(L"BUTTON", L"删除容器", BS_PUSHBUTTON, bx, 122, bw, bh, ID_BTN_DELETE_CTX);
                    ctl(L"BUTTON", L"应用样式", BS_PUSHBUTTON, bx, 150, bw, bh, ID_BTN_STYLE);
                    ctl(L"BUTTON", L"快捷方式", BS_GROUPBOX, 10, 280, 340, 200, 0);
                    hListSc = ctl(L"LISTBOX", L"", WS_BORDER|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
                                 20, 300, 198, 156, ID_LIST_SHORTCUTS);
                    ctl(L"BUTTON", L"添加", BS_PUSHBUTTON, bx, 300, bw, bh, ID_BTN_ADD_SHORTCUT);
                    ctl(L"BUTTON", L"删除", BS_PUSHBUTTON, bx, 330, bw, bh, ID_BTN_DEL_SC);
                    ctl(L"BUTTON", L"容器图标", BS_GROUPBOX, 360, 10, 220, 210, 0);
                    hIconPreview = ctl(L"STATIC", L"", SS_ICON|SS_CENTERIMAGE|SS_REALSIZEIMAGE,
                                      375, 60, 190, 130, ID_ICON_PREVIEW);
                    ctl(L"BUTTON", L"关闭", BS_PUSHBUTTON | BS_DEFPUSHBUTTON, 500, 466, 84, 28, ID_BTN_CLOSE);
                    RefreshContainers();
                    if (!g_containers.empty()) SendMessageW(hListCtx, LB_SETCURSEL, 0, 0);
                    RefreshShortcuts(); SyncStyleCombo(); RefreshIconPreview(); UpdateButtons(h);
                    return 0;
                }
                case WM_COMMAND: {
                    int id = LOWORD(w); HWND hSrc = (HWND)l;
                    if (hSrc == hListCtx) {
                        if (HIWORD(w) == LBN_SELCHANGE) { RefreshShortcuts(); SyncStyleCombo(); RefreshIconPreview(); UpdateButtons(h); }
                        else if (HIWORD(w) == LBN_DBLCLK) {
                            int ci = SelectedContainerIndex();
                            if (ci >= 0 && (size_t)ci < g_containers.size()) {
                                g_renameIdx = ci;
                                SetWindowTextW(hEditName, g_containers[ci]->Data().name.c_str());
                                SetFocus(hEditName);
                                SendMessageW(hEditName, EM_SETSEL, 0, -1);
                            }
                        }
                    } else if (hSrc == hListSc) {
                        if (HIWORD(w) == LBN_SELCHANGE) UpdateButtons(h);
                    } else if (hSrc == hEditName) {
                        // 退出编辑状态即保存；g_renameIdx 保护防止重复提交
                        if (HIWORD(w) == EN_KILLFOCUS || (HIWORD(w) == 0 && id == ID_EDIT_NAME)) {
                            if (g_renameIdx >= 0) CommitRename(h);
                        }
                    } else switch (id) {
                        case ID_BTN_NEW: {
                            CreateContainer();
                            RefreshContainers();
                            if (!g_containers.empty()) SendMessageW(hListCtx, LB_SETCURSEL, (WPARAM)(g_containers.size()-1), 0);
                            RefreshShortcuts(); SyncStyleCombo(); RefreshIconPreview(); UpdateButtons(h);
                            break;
                        }
                        case ID_BTN_CHANGE_ICON: {
                            int ci = SelectedContainerIndex();
                            if (ci < 0) break;
                            std::wstring iconFile;
                            if (PickIconFile(h, iconFile) && !iconFile.empty()) {
                                g_containers[ci]->Data().iconPath = iconFile;
                                g_containers[ci]->Render(); Config::MarkDirty();
                                RefreshIconPreview(); UpdateButtons(h);
                            }
                            break;
                        }
                        case ID_BTN_DELETE_CTX: {
                            int ci = SelectedContainerIndex();
                            if (ci < 0) break;
                            for (auto& e : g_containers[ci]->Data().shortcuts) {
                                std::wstring t = e.path;
                                if (e.type == "lnk") { std::wstring tt = desktop::LnkTarget(e.path); if (!tt.empty()) t = tt; }
                                desktop::CreateDesktopShortcut(e.name, t);
                            }
                            g_containers.erase(g_containers.begin() + ci);
                            Config::MarkDirty();
                            RefreshContainers(); RefreshShortcuts(); SyncStyleCombo(); RefreshIconPreview(); UpdateButtons(h);
                            break;
                        }
                        case ID_BTN_STYLE: {
                            int ci = SelectedContainerIndex();
                            int sel = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
                            if (ci < 0 || sel == CB_ERR) break;
                            g_containers[ci]->Data().style = (ExpandStyle)sel;
                            Config::MarkDirty(); RefreshContainers(); SyncStyleCombo();
                            if ((size_t)ci < g_containers.size()) SendMessageW(hListCtx, LB_SETCURSEL, ci, 0);
                            break;
                        }
                        case ID_BTN_ADD_SHORTCUT: {
                            int ci = SelectedContainerIndex();
                            if (ci < 0) break;
                            std::wstring picked; bool folder=false;
                            if (PickPath(h, picked, folder)) {
                                ShortcutEntry e;
                                e.path = picked; e.type = util::GuessType(picked);
                                size_t sep = picked.find_last_of((wchar_t)92);   // 92 = backslash
                                e.name = picked.substr(sep + 1);
                                if (e.name.size() >= 4 && e.name.rfind(L".lnk") == e.name.size()-4)
                                    e.name = e.name.substr(0, e.name.size()-4);
                                else { size_t dot = e.name.find_last_of(L'.'); if (dot != std::wstring::npos) e.name = e.name.substr(0, dot); }
                                g_containers[ci]->Data().shortcuts.push_back(e);
                                g_containers[ci]->Render(); Config::MarkDirty();
                            }
                            RefreshShortcuts(); UpdateButtons(h);
                            break;
                        }
                        case ID_BTN_DEL_SC: {
                            int ci = SelectedContainerIndex();
                            int si = (int)SendMessageW(hListSc, LB_GETCURSEL, 0, 0);
                            if (ci < 0 || si == LB_ERR || (size_t)si >= g_containers[ci]->Data().shortcuts.size()) break;
                            g_containers[ci]->Data().shortcuts.erase(g_containers[ci]->Data().shortcuts.begin() + si);
                            g_containers[ci]->Render(); Config::MarkDirty();
                            RefreshShortcuts(); UpdateButtons(h);
                            break;
                        }
                        case ID_BTN_CLOSE:
                        case IDCANCEL:
                            DestroyWindow(h); break;
                    }
                    return 0;
                }
                case WM_DESTROY:
                    if (g_previewIcon) { DestroyIcon(g_previewIcon); g_previewIcon = nullptr; }
                    return 0;
            }
            return DefWindowProcW(h, m, w, l);
        };

        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DeskStackManager";
        RegisterClassExW(&wc);
        registered = true;
    }
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DeskStackManager", L"DeskStack 管理界面",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                0, 0, 600, 500, owner, nullptr, hInst, nullptr);
    if (!hwnd) return;
    {
        RECT wr; GetWindowRect(hwnd, &wr);
        int cx = (GetSystemMetrics(SM_CXSCREEN) - (wr.right - wr.left)) / 2;
        int cy = (GetSystemMetrics(SM_CYSCREEN) - (wr.bottom - wr.top)) / 2;
        SetWindowPos(hwnd, nullptr, cx, cy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        // 重命名编辑框中按 Enter：提交并退出编辑（避免触发默认关闭按钮）
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && GetFocus() == hEditName) {
            if (g_renameIdx >= 0) CommitRename(hwnd);
            SetFocus(hListCtx);
            continue;
        }
        if (IsDialogMessageW(hwnd, &msg)) continue;
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { DestroyWindow(hwnd); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
