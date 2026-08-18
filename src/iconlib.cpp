#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "iconlib.h"
#include "desktop.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shlguid.h>

namespace iconlib {

// 兜底：手工构造一个简单的实心方块图标（系统图标不可用时的最后手段，保证非空）
HICON CreateDummyIcon(int size) {
    int s = size ? size : 32;
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, scr);
    if (!dib) { DeleteDC(mem); return nullptr; }
    HBITMAP old = (HBITMAP)SelectObject(mem, dib);
    HBRUSH br = CreateSolidBrush(RGB(0x40, 0x80, 0xFF));
    HBRUSH oldbr = (HBRUSH)SelectObject(mem, br);
    RoundRect(mem, 4, 4, s - 4, s - 4, s / 4, s / 4);
    SelectObject(mem, oldbr);
    DeleteObject(br);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE; ii.hbmColor = dib; ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    SelectObject(mem, old);
    DeleteObject(mask); DeleteObject(dib); DeleteDC(mem);
    return ic;
}

HICON DefaultIcon(int size) {
    HICON sys = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                                  size ? size : 32, size ? size : 32, LR_DEFAULTSIZE);
    if (!sys) sys = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                                      size ? size : 32, size ? size : 32, 0);
    if (!sys) sys = CreateDummyIcon(size);
    return sys;
}

static bool EndsWith(const std::wstring& s, const wchar_t* ext) {
    size_t n = wcslen(ext);
    if (s.size() < n) return false;
    return _wcsicmp(s.c_str() + s.size() - n, ext) == 0;
}

// 解析 .lnk 指向的实际目标路径（找不到则返回空）
static std::wstring ResolveLnk(const std::wstring& lnk) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) return {};
    IPersistFile* pf = nullptr;
    if (FAILED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { sl->Release(); return {}; }
    HRESULT hr = pf->Load(lnk.c_str(), STGM_READ);
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

// 从 exe/dll/ico 直接提取内嵌图标（不依赖 shell，更可靠）
static HICON ExtractExeIcon(const std::wstring& path, int size) {
    HICON large = nullptr, smallIcon = nullptr;
    int n = ExtractIconExW(path.c_str(), 0, &large, &smallIcon, 1);
    if (n > 0 && large) return large;
    if (n > 0 && smallIcon) return smallIcon;
    return nullptr;
}

// 从实际路径获取关联图标（SHGetFileInfo）
static HICON ShellIcon(const std::wstring& path, int size) {
    DWORD attr = GetFileAttributesW(path.c_str());
    UINT flags = SHGFI_ICON;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) flags |= SHGFI_USEFILEATTRIBUTES;
    SHFILEINFOW sfi = {};
    UINT fi = attr & FILE_ATTRIBUTE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (SHGetFileInfoW(path.c_str(), fi, &sfi, sizeof(sfi), flags))
        if (sfi.hIcon) return sfi.hIcon;
    return nullptr;
}

HICON IconForPath(const std::wstring& path, int size) {
    // 1) 尽快解析 .lnk 的真实目标，从目标本身取图标
    if (desktop::IsLnk(path)) {
        std::wstring target = ResolveLnk(path);
        if (!target.empty()) {
            HICON ic = ExtractExeIcon(target, size);
            if (ic) return ic;
            ic = ShellIcon(target, size);
            if (ic) return ic;
        }
        // .lnk 目标解析失败时，退而取 .lnk 自身关联图标
        HICON ic = ShellIcon(path, size);
        if (ic) return ic;
    }

    // 2) .ico 文件：直接载入图片内容
    if (EndsWith(path, L".ico")) {
        HICON ic = (HICON)LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                                     size, size, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (ic) return ic;
    }

    // 3) 存在实际文件/目录：先试 exe 内嵌图标，再用 ShellIcon
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            HICON ic = ExtractExeIcon(path, size);
            if (ic) return ic;
        }
        HICON sh = ShellIcon(path, size);
        if (sh) return sh;
    }

    // 4) 最后兜底
    HICON fallback = DefaultIcon(size);
    if (fallback) return fallback;
    return CreateDummyIcon(size);
}

} // namespace iconlib
