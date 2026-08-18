#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include "container.h"
#include "panel.h"
#include "config.h"
#include "manager.h"
#include "desktop.h"
#include "iconlib.h"
#include "layered.h"
#include "appstate.h"
#include "resource.h"
#include <vector>
#include <memory>
#include <algorithm>

std::vector<std::unique_ptr<ContainerWindow>> g_containers;
HWND g_owner = nullptr;

#define WM_TRAYICON (WM_APP + 1)

static UINT g_taskbarCreated = 0;

void RestoreAllContainers() {
    for (auto& c : g_containers)
        if (c->Hwnd()) ShowWindow(c->Hwnd(), SW_SHOW);
}

static void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hwnd; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"DeskStack 桌面折叠容器");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RevealConfigFile() {
    Config::SaveNow();
    std::wstring args = L"/select,\"" + Config::Path() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void ShowTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 10, L"管理界面");
    AppendMenuW(menu, MF_STRING, 12, L"恢复全部悬浮容器");
    AppendMenuW(menu, MF_STRING, 13, L"移除所有容器");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 14, L"打开配置文件位置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 15, L"退出程序");
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == 10)      OpenManagerWindow(hwnd);
    else if (cmd == 12) RestoreAllContainers();
    else if (cmd == 13) Config::RemoveAll();
    else if (cmd == 14) RevealConfigFile();
    else if (cmd == 15) { Config::SaveNow(); PostQuitMessage(0); }
}

// Explorer 重启：桌面树重建，容器子窗口随旧树销毁 → 从配置重建
static void RebuildAll() {
    Config::SaveNow();
    g_containers.clear();
    Config::LoadApp();
}

static bool AnyContainerDead() {
    for (auto& c : g_containers)
        if (!c->Hwnd() || !IsWindow(c->Hwnd())) return true;
    return false;
}

LRESULT CALLBACK OwnerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) { RebuildAll(); return 0; }
    switch (msg) {
        case WM_CREATE: {
            AddTrayIcon(hwnd);
            RegisterShellHookWindow(hwnd);
            return 0;
        }
        case WM_TRAYICON:
            if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu(hwnd);
            return 0;
        case WM_CONTAINER_DELETE: {
            HWND target = (HWND)wp;
            auto it = std::find_if(g_containers.begin(), g_containers.end(),
                [target](auto& c) { return c->Hwnd() == target; });
            if (it != g_containers.end()) { g_containers.erase(it); Config::MarkDirty(); }
            return 0;
        }
        case WM_OPEN_MANAGER:
            OpenManagerWindow(hwnd);
            return 0;
        case WM_REMOVE_ALL:
            Config::RemoveAll();
            return 0;
        case WM_SHOW_ALL:
            RestoreAllContainers();
            return 0;
        case WM_TIMER:
            if (wp == 3) { Config::SaveNow(); return 0; }   // 配置防抖写盘
            if (wp == 4) {                                  // 桌面轮询：仅处理 Explorer 重启后的重挂，不干扰拖拽
                if (AnyContainerDead()) RebuildAll();
                else {
                    HWND host = desktop::FindDesktopListView();
                    if (host)
                        for (auto& c : g_containers)
                            if (GetParent(c->Hwnd()) != host || !IsWindowVisible(c->Hwnd())) {
                                // 宿主变化或尚未显示 → 重挂、置顶、按网格定位并显示
                                SetParent(c->Hwnd(), host);
                                SetWindowPos(c->Hwnd(), HWND_TOP, 0, 0, 0, 0,
                                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                                c->ReapplyPosition();
                            }
                }
            }
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            // DPI/图标大小变化：重新读取网格并重定位
            for (auto& c : g_containers) c->ReapplyPosition();
            return 0;
        case WM_DESTROY:
            Config::SaveNow();
            Shell_NotifyIconW(NIM_DELETE, &NOTIFYICONDATAW{ sizeof(NOTIFYICONDATAW), hwnd, 1 });
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}


int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"DeskStack_SingleInstance");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    desktop::EnableDpiAwareness();
    layered::GlobalInit();
    OleInitialize(nullptr);
    InitCommonControls();

    ContainerWindow::RegisterClass(hInst);
    PanelWindow::RegisterClass(hInst);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = OwnerWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"DeskStackOwner";
    RegisterClassExW(&wc);

    g_owner = CreateWindowExW(0, L"DeskStackOwner", nullptr, WS_POPUP,
                              0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (!Config::LoadApp()) {
        // 首次运行：创建一个示例容器
        ContainerData d;
        d.id = "c1"; d.name = L"开发工具";
        WORD cw=0,ch=0; desktop::GetCellSize(cw,ch);
        d.col = 1; d.row = 0;
        auto c = std::make_unique<ContainerWindow>(d);
        if (c->Hwnd()) g_containers.push_back(std::move(c));
    }

    SetTimer(g_owner, 4, 300, nullptr);   // 桌面/Explorer 轮询

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_containers.clear();
    OleUninitialize();
    layered::GlobalShutdown();
    return 0;
}
