#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

// ── Shared data model ────────────────────────────────────────────────
// UI 全中文；配置内部字段保存英文枚举。

enum class ExpandStyle { Grid, Column, Row, Fan, Ring };

struct ShortcutEntry {
    std::wstring name;      // 显示名
    std::wstring path;      // 目标路径 (exe / lnk / 文件夹)
    std::string  type;      // "exe" / "lnk" / "folder"（由程序自动识别，可省略）
};

struct ContainerData {
    std::string   id;               // 唯一标识 (ascii)
    std::wstring  name;             // 显示名，可重复
    std::wstring  iconPath;         // 容器图标（exe/lnk/ico/png），可空
    int           col = 0;          // 网格索引（列）
    int           row = 0;          // 网格索引（行）
    ExpandStyle   style = ExpandStyle::Grid;
    int           gridCols = 5;     // 网格列数
    std::vector<ShortcutEntry> shortcuts;
};

// ── 工具函数 ────────────────────────────────────────────────────────
namespace util {

inline std::string  ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n,
                        nullptr, nullptr);
    return s;
}

inline std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// 当前进程系统 DPI 缩放因子（相对 96）
inline float DpiScale() {
    HDC hdc = GetDC(nullptr);
    float s = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, hdc);
    return s;
}

inline int Scaled(int v) { return (int)(v * DpiScale() + 0.5f); }

inline std::wstring StyleToChinese(ExpandStyle st) {
    switch (st) {
        case ExpandStyle::Grid:   return L"网格";
        case ExpandStyle::Column: return L"单列";
        case ExpandStyle::Row:    return L"单行";
        case ExpandStyle::Fan:    return L"扇形";
        case ExpandStyle::Ring:   return L"环形";
    }
    return L"网格";
}

inline std::string StyleToName(ExpandStyle st) {
    switch (st) {
        case ExpandStyle::Grid:   return "Grid";
        case ExpandStyle::Column: return "Column";
        case ExpandStyle::Row:    return "Row";
        case ExpandStyle::Fan:    return "Fan";
        case ExpandStyle::Ring:   return "Ring";
    }
    return "Grid";
}

inline ExpandStyle StyleFromName(const std::string& n) {
    if (n == "Column") return ExpandStyle::Column;
    if (n == "Row")    return ExpandStyle::Row;
    if (n == "Fan")    return ExpandStyle::Fan;
    if (n == "Ring")   return ExpandStyle::Ring;
    return ExpandStyle::Grid;
}

// 根据路径自动识别条目类型
inline std::string GuessType(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return "folder";
    std::wstring lower = path;
    for (auto& c : lower) if (c >= L'A' && c <= L'Z') c += (L'a' - L'A');
    if (lower.size() >= 4 && lower.rfind(L".lnk") == lower.size() - 4)
        return "lnk";
    if (lower.size() >= 4 && lower.rfind(L".exe") == lower.size() - 4)
        return "exe";
    if (lower.size() >= 4 && lower.rfind(L".bat") == lower.size() - 4)
        return "exe";
    return "folder";
}

} // namespace util
