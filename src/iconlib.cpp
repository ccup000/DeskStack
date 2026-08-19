#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "iconlib.h"
#include "desktop.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shlguid.h>
#include <vector>
#include <gdiplus.h>

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
    if (size <= 0) size = 32;
    // 先按请求尺寸加载，再退回系统默认尺寸，避免 48px 请求返回 32px 的兜底图标。
    HICON sys = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                                  size, size, 0);
    if (!sys) sys = (HICON)LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON,
                                      size, size, LR_DEFAULTSIZE);
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

// 判断 .lnk 是否为 UWP 快捷方式：GetPath 为空且 GetIconLocation 为空。
bool IsUwpShortcut(const std::wstring& path) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) return false;
    IPersistFile* pf = nullptr;
    if (FAILED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { sl->Release(); return false; }
    HRESULT hr = pf->Load(path.c_str(), STGM_READ);
    pf->Release();
    if (FAILED(hr)) { sl->Release(); return false; }
    wchar_t target[MAX_PATH] = {};
    WIN32_FIND_DATAW fd = {};
    HRESULT hp = sl->GetPath(target, MAX_PATH, &fd, SLGP_RAWPATH);
    wchar_t iconLoc[MAX_PATH] = {};
    int iconIdx = 0;
    HRESULT hi = sl->GetIconLocation(iconLoc, MAX_PATH, &iconIdx);
    sl->Release();
    // UWP 快捷方式通常没有目标路径，也没有图标位置
    if (FAILED(hp) || target[0] == L'\0') {
        if (FAILED(hi) || iconLoc[0] == L'\0')
            return true;
    }
    return false;
}

// 从 .lnk 的 IconLocation 直接加载图标（支持 .ico、exe/dll 及资源索引）。
// 这对“手动修改了显示图标”的快捷方式最可靠。
static HICON IconFromLnkLocation(const std::wstring& lnkPath, int size) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) return nullptr;
    IPersistFile* pf = nullptr;
    if (FAILED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { sl->Release(); return nullptr; }
    HRESULT hr = pf->Load(lnkPath.c_str(), STGM_READ);
    pf->Release();
    if (FAILED(hr)) { sl->Release(); return nullptr; }

    wchar_t iconLoc[MAX_PATH] = {};
    int iconIdx = 0;
    HRESULT hi = sl->GetIconLocation(iconLoc, MAX_PATH, &iconIdx);
    sl->Release();
    if (FAILED(hi) || !iconLoc[0]) return nullptr;

    HICON ic = nullptr;
    // 若指向 .ico，优先直接加载
    std::wstring loc(iconLoc);
    std::wstring lower = loc;
    for (auto& ch : lower) if (ch >= L'A' && ch <= L'Z') ch += (L'a' - L'A');
    if (lower.size() >= 4 && lower.rfind(L".ico") == lower.size() - 4) {
        ic = (HICON)LoadImageW(nullptr, iconLoc, IMAGE_ICON,
                               size, size, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    if (!ic) {
        HICON large = nullptr, smallIcon = nullptr;
        int n = ExtractIconExW(iconLoc, iconIdx, &large, &smallIcon, 1);
        if (n > 0 && large) ic = large;
        else if (n > 0 && smallIcon) ic = smallIcon;
        else if (large) ic = large;
        else if (smallIcon) ic = smallIcon;
    }
    return ic;
}

// 从 exe/dll/ico 直接提取内嵌图标（不依赖 shell，更可靠）
static HICON ExtractExeIcon(const std::wstring& path, int size) {
    (void)size;   // 实际提取不依赖请求尺寸，保留参数以统一调用签名
    HICON large = nullptr, smallIcon = nullptr;
    int n = ExtractIconExW(path.c_str(), 0, &large, &smallIcon, 1);
    if (n > 0 && large) return large;
    if (n > 0 && smallIcon) return smallIcon;
    return nullptr;
}

// 从实际路径获取关联图标（SHGetFileInfo）
static HICON ShellIcon(const std::wstring& path, int size) {
    (void)size;   // SHGetFileInfo 不接收请求尺寸，保留参数以统一调用签名
    DWORD attr = GetFileAttributesW(path.c_str());
    UINT flags = SHGFI_ICON;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) flags |= SHGFI_USEFILEATTRIBUTES;
    SHFILEINFOW sfi = {};
    UINT fi = attr & FILE_ATTRIBUTE_DIRECTORY ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (SHGetFileInfoW(path.c_str(), fi, &sfi, sizeof(sfi), flags))
        if (sfi.hIcon) return sfi.hIcon;
    return nullptr;
}

// 把 32bpp 位图转换为 HICON（IShellItemImageFactory 返回 HBITMAP，调用方需要 HICON）。
// CreateIconIndirect 创建的图标在 GDI+ Bitmap::FromHICON 中会以掩码判断透明，
// 因此这里用 alpha 通道生成掩码：alpha==0 的像素置为透明，其余置为不透明。
static HICON BitmapToIcon(HBITMAP bmp) {
    if (!bmp) return nullptr;
    BITMAP bm = {};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return nullptr;
    int w = bm.bmWidth, h = bm.bmHeight;

    HDC hdc = GetDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down，便于按行读 alpha
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> px((size_t)w * h * 4);
    int lines = GetDIBits(hdc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (lines != h) return nullptr;

    HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr);
    if (!mask) return nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(mem, mask);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            BYTE a = px[((size_t)y * w + x) * 4 + 3];
            // 单色位图中：白(1)=透明，黑(0)=不透明
            SetPixelV(mem, x, y, a == 0 ? RGB(255, 255, 255) : RGB(0, 0, 0));
        }
    }
    SelectObject(mem, old);
    DeleteDC(mem);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    return icon;
}

// 通过 shell 的 IShellItemImageFactory 获取图标。
// 优点：对 .lnk（含 UWP 快捷方式、自定义 .ico 图标）和 WindowsApps 下无读取权限的 exe
// 都能按 shell 显示效果返回图标。
static HICON IconFromShellItemImageFactory(const std::wstring& path, int size) {
    // SHCreateItemFromParsingName 不接受正斜杠；配置里的路径可能是混合分隔符，
    // 统一成反斜杠再解析，否则会 E_INVALIDARG。
    std::wstring norm = path;
    for (auto& c : norm) if (c == L'/') c = L'\\';
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(norm.c_str(), nullptr,
                                             IID_IShellItem, (void**)&item);
    if (FAILED(hr) || !item) return nullptr;
    IShellItemImageFactory* factory = nullptr;
    hr = item->QueryInterface(IID_IShellItemImageFactory, (void**)&factory);
    item->Release();
    if (FAILED(hr) || !factory) return nullptr;

    SIZE sz = { size, size };
    HBITMAP bmp = nullptr;
    hr = factory->GetImage(sz, SIIGBF_ICONONLY, &bmp);
    factory->Release();
    if (FAILED(hr) || !bmp) return nullptr;

    HICON icon = BitmapToIcon(bmp);
    DeleteObject(bmp);
    return icon;
}

// 从 shell 的 IShellItemImageFactory 获取高分辨率 32bpp ARGB 位图。
// 直接保留 alpha 通道，避免转 HICON 时用单色掩码造成边缘锯齿。
static Gdiplus::Bitmap* BitmapFromShellItemFactory(const std::wstring& path, int size) {
    std::wstring norm = path;
    for (auto& c : norm) if (c == L'/') c = L'\\';
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(norm.c_str(), nullptr,
                                             IID_IShellItem, (void**)&item);
    if (FAILED(hr) || !item) return nullptr;
    IShellItemImageFactory* factory = nullptr;
    hr = item->QueryInterface(IID_IShellItemImageFactory, (void**)&factory);
    item->Release();
    if (FAILED(hr) || !factory) return nullptr;

    SIZE sz = { size, size };
    HBITMAP bmp = nullptr;
    hr = factory->GetImage(sz, SIIGBF_ICONONLY, &bmp);
    factory->Release();
    if (FAILED(hr) || !bmp) return nullptr;

    BITMAP bm = {};
    if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        DeleteObject(bmp);
        return nullptr;
    }
    int w = bm.bmWidth, h = bm.bmHeight;

    HDC hdc = GetDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> px((size_t)w * h * 4);
    int lines = GetDIBits(hdc, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    DeleteObject(bmp);
    if (lines != h) return nullptr;

    Gdiplus::Bitmap* out = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
    if (!out) return nullptr;
    Gdiplus::BitmapData bd;
    if (out->LockBits(&Gdiplus::Rect(0, 0, w, h),
                      Gdiplus::ImageLockModeWrite,
                      PixelFormat32bppARGB, &bd) == Gdiplus::Ok) {
        memcpy(bd.Scan0, px.data(), (size_t)w * h * 4);
        out->UnlockBits(&bd);
    }
    return out;
}

// 从 .lnk 的 IconLocation 直接返回 GDI+ Bitmap（仅支持 .ico，保留 alpha 透明通道）。
static Gdiplus::Bitmap* BitmapFromLnkIconLocation(const std::wstring& lnkPath) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl))) return nullptr;
    IPersistFile* pf = nullptr;
    if (FAILED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { sl->Release(); return nullptr; }
    HRESULT hr = pf->Load(lnkPath.c_str(), STGM_READ);
    pf->Release();
    if (FAILED(hr)) { sl->Release(); return nullptr; }
    wchar_t iconLoc[MAX_PATH] = {};
    int iconIdx = 0;
    HRESULT hi = sl->GetIconLocation(iconLoc, MAX_PATH, &iconIdx);
    sl->Release();
    if (FAILED(hi) || !iconLoc[0]) return nullptr;

    std::wstring loc(iconLoc);
    std::wstring lower = loc;
    for (auto& ch : lower) if (ch >= L'A' && ch <= L'Z') ch += (L'a' - L'A');
    if (lower.size() >= 4 && lower.rfind(L".ico") == lower.size() - 4)
        return Gdiplus::Bitmap::FromFile(iconLoc);
    return nullptr;
}

HICON IconForPath(const std::wstring& path, int size) {
    // 1) .lnk：
    //    优先读取快捷方式 IconLocation 直接加载（手动指定 .ico/exe 时最准确）；
    //    其次 SHGetFileInfo；最后才用 IShellItemImageFactory（UWP 可能返回黑色占位图）。
    if (desktop::IsLnk(path)) {
        HICON ic = IconFromLnkLocation(path, size);
        if (ic) return ic;
        ic = ShellIcon(path, size);
        if (ic) return ic;
        ic = IconFromShellItemImageFactory(path, size);
        if (ic) return ic;
        // 快捷方式自身图标取不到时，再解析真实目标，从目标本身取图标
        std::wstring target = ResolveLnk(path);
        if (!target.empty()) {
            ic = ExtractExeIcon(target, size);
            if (ic) return ic;
            ic = ShellIcon(target, size);
            if (ic) return ic;
        }
    }

    // 2) .ico 文件：直接载入图片内容
    if (EndsWith(path, L".ico")) {
        HICON ic = (HICON)LoadImageW(nullptr, path.c_str(), IMAGE_ICON,
                                     size, size, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (ic) return ic;
    }

    // 3) 存在实际文件/目录：先试 exe 内嵌图标，再试 shell 项图像工厂，最后 ShellIcon。
    //    WindowsApps 等目录下 GetFileAttributes 可能成功，但 ExtractIconEx 会因权限失败；
    //    此时 IShellItemImageFactory 仍能按 shell 显示效果返回正确图标。
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            HICON ic = ExtractExeIcon(path, size);
            if (ic) return ic;
            ic = IconFromShellItemImageFactory(path, size);
            if (ic) return ic;
        }
        HICON sh = ShellIcon(path, size);
        if (sh) return sh;
    }

    // 3.5) WindowsApps 等目录下文件可能无读取权限（GetFileAttributes 失败），
    //       但 shell 仍能通过 IShellItemImageFactory 显示其图标。
    {
        HICON ic = IconFromShellItemImageFactory(path, size);
        if (ic) return ic;
    }

    // 4) 最后兜底
    HICON fallback = DefaultIcon(size);
    if (fallback) return fallback;
    return CreateDummyIcon(size);
}

Gdiplus::Bitmap* IconBitmapForPath(const std::wstring& path, int size) {
    // .lnk 若指定了 .ico 图标，直接用 GDI+ 加载该 .ico，保留透明通道。
    if (desktop::IsLnk(path)) {
        Gdiplus::Bitmap* icoBmp = BitmapFromLnkIconLocation(path);
        if (icoBmp) return icoBmp;

        // 其他 .lnk 优先走 HICON 路径，以便尊重快捷方式自定义图标；
        // shell 工厂对 UWP/自定义 .ico 快捷方式可能返回黑色占位图，不能作为 .lnk 首选。
        HICON ic = IconForPath(path, size);
        if (ic) {
            Gdiplus::Bitmap* out = Gdiplus::Bitmap::FromHICON(ic);
            DestroyIcon(ic);
            if (out) return out;
        }
    }

    // 非 .lnk 或 .lnk 走 HICON 失败时，再请求高分辨率 shell 位图。
    int hiSize = size * 2;
    if (hiSize < 96) hiSize = 96;
    if (hiSize > 256) hiSize = 256;
    Gdiplus::Bitmap* bmp = BitmapFromShellItemFactory(path, hiSize);
    if (bmp) return bmp;

    // 最后再回退一次 HICON 路径
    HICON ic = IconForPath(path, size);
    if (ic) {
        Gdiplus::Bitmap* out = Gdiplus::Bitmap::FromHICON(ic);
        DestroyIcon(ic);
        return out;
    }
    return nullptr;
}

} // namespace iconlib
