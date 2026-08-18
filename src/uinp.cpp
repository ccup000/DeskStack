#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "uinp.h"

namespace {
std::wstring* g_result = nullptr;   // 通过静态保存经 WndProc 返回输入
bool g_ok = false;

LRESULT CALLBACK InputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = (CREATESTRUCTW*)lp;
            auto* p = (std::pair<std::wstring, std::wstring>*)cs->lpCreateParams;
            // 标签
            CreateWindowExW(0, L"STATIC", p->first.c_str(), WS_CHILD | WS_VISIBLE,
                            14, 12, 260, 20, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            // 输入框
            HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", p->second.c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      14, 40, 260, 24, hwnd, (HMENU)100,
                                      GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ed, EM_SETSEL, 0, -1);
            SetFocus(ed);
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDOK || id == IDCANCEL) {
                HWND ed = GetDlgItem(hwnd, 100);
                wchar_t buf[512] = {};
                if (id == IDOK && ed) {
                    GetWindowTextW(ed, buf, 512);
                    if (g_result) *g_result = buf;
                }
                g_ok = (id == IDOK);
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            // 白底黑字，与对话框背景一致
            HDC hdc = (HDC)wp;
            SetBkColor(hdc, RGB(240, 240, 240));
            return (LRESULT)GetStockObject(WHITE_BRUSH);
        }
        // WM_DESTROY 不再 PostQuitMessage：嵌套对话框关闭后只结束自己的模态循环，
        // 不让 WM_QUIT 传导到主消息循环（否则整个程序会退出/看似崩溃）。
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool ShowInputBox(HWND owner, const std::wstring& title,
                  const std::wstring& prompt, const std::wstring& initial,
                  std::wstring& result) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = InputWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DeskStackInput";
        RegisterClassExW(&wc);
        registered = true;
    }
    std::wstring tmp = initial;
    g_result = &tmp;
    g_ok = false;
    std::pair<std::wstring, std::wstring> p{ prompt, initial };

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"DeskStackInput", title.c_str(),
                               WS_CAPTION | WS_SYSMENU | WS_POPUP,
                               CW_USEDEFAULT, CW_USEDEFAULT, 300, 120,
                               owner, nullptr, hInst, &p);
    if (!dlg) return false;
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    // 模态循环
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(dlg, WM_COMMAND, IDOK, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_ok) result = tmp;
    return g_ok;
}
