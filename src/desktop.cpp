#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "desktop.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <vector>

namespace desktop {

void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI* SetCtxT)(HANDLE);
        auto setCtx = (SetCtxT)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI* SetDpiT)(int);
        auto set = (SetDpiT)(void*)GetProcAddress(shcore, "SetProcessDpiAwareness");
        HRESULT hr = set ? set(2) : E_FAIL;
        FreeLibrary(shcore);
        if (SUCCEEDED(hr)) return;
    }
    SetProcessDPIAware();
}

static HWND ListViewOf(HWND shell) {
    if (!shell) return nullptr;
    for (HWND dv = FindWindowExW(shell, nullptr, L"SHELLDLL_DefView", nullptr); dv;
         dv = FindWindowExW(shell, dv, L"SHELLDLL_DefView", nullptr)) {
        HWND lv = FindWindowExW(dv, nullptr, L"SysListView32", nullptr);
        if (lv) return lv;
    }
    return nullptr;
}

static HWND FindDesktopLVOnce() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    if (HWND lv = ListViewOf(progman)) return lv;
    for (HWND ww = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(progman, ww, L"WorkerW", nullptr))
        if (HWND lv = ListViewOf(ww)) return lv;
    for (HWND ww = FindWindowExW(nullptr, nullptr, L"WorkerW", nullptr); ww;
         ww = FindWindowExW(nullptr, ww, L"WorkerW", nullptr))
        if (HWND lv = ListViewOf(ww)) return lv;
    return progman;
}

HWND FindDesktopListView() {
    HWND lv = FindDesktopLVOnce();
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (lv && lv != progman) return lv;
    if (progman) {
        SendMessageW(progman, 0x052C, 0, 0);
        Sleep(80);
        lv = FindDesktopLVOnce();
        if (lv && lv != progman) return lv;
    }
    return lv;
}

bool GetCellSize(WORD& cellW, WORD& cellH) {
    HWND lv = FindDesktopListView();
    if (!lv) return false;
    LRESULT r = SendMessageW(lv, LVM_GETITEMSPACING, TRUE, 0);
    if ((int)r == 0) return false;
    cellW = LOWORD(r);
    cellH = HIWORD(r);
    return true;
}

POINT ScreenToClientOf(HWND hwnd, POINT pt) {
    ScreenToClient(hwnd, &pt);
    return pt;
}

int IconSize() {
    int s = GetSystemMetrics(SM_CXICON);
    if (s <= 0) s = 32;
    return s;
}

std::wstring DesktopFolder() {
    wchar_t desktop[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop)))
        return {};
    return desktop;
}

bool IsLnk(const std::wstring& path) {
    std::wstring p = path;
    for (auto& c : p) if (c >= L'A' && c <= L'Z') c += (L'a' - L'A');
    return p.size() >= 4 && p.rfind(L".lnk") == p.size() - 4;
}

std::wstring LnkTarget(const std::wstring& lnkPath) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) return {};
    IPersistFile* pf = nullptr;
    if (FAILED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { sl->Release(); return {}; }
    HRESULT hr = pf->Load(lnkPath.c_str(), STGM_READ);
    pf->Release();
    if (FAILED(hr)) { sl->Release(); return {}; }
    wchar_t target[MAX_PATH] = {};
    WIN32_FIND_DATAW fd = {};
    if (SUCCEEDED(sl->GetPath(target, MAX_PATH, &fd, SLGP_RAWPATH)) && target[0]) {
        sl->Release();
        return target;
    }
    sl->Release();
    return {};
}

bool CreateDesktopShortcut(const std::wstring& name, const std::wstring& target) {
    if (target.empty()) return false;
    std::wstring folder = DesktopFolder();
    if (folder.empty()) return false;
    std::wstring lnkPath = folder + L"\\" + name + L".lnk";

    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str());
    if (GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_DIRECTORY)
        sl->SetWorkingDirectory(target.c_str());
    IPersistFile* pf = nullptr;
    HRESULT hr = E_FAIL;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        hr = pf->Save(lnkPath.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
    return SUCCEEDED(hr);
}

} // namespace desktop
