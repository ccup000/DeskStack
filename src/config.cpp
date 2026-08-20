#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "config.h"
#include "types.h"
#include "container.h"
#include "desktop.h"
#include "appstate.h"
#include "nlohmann/json.hpp"
#include <shlobj.h>
#include <fstream>
#include <vector>
#include <memory>

using json = nlohmann::json;

namespace {

const std::wstring& Dir() {
    static const std::wstring d = [] {
        wchar_t appdata[MAX_PATH] = {};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
            return std::wstring(L"C:\\");
        return std::wstring(appdata) + L"/DeskStack";
    }();
    return d;
}

std::wstring AsW(const json& j, const char* key) {
    return j.contains(key) && j[key].is_string()
        ? util::FromUtf8(j[key].get<std::string>()) : std::wstring();
}

json SaveOne(const ContainerWindow& c) {
    const auto& d = c.Data();
    json j;
    j["id"]         = d.id;
    j["name"]       = util::ToUtf8(d.name);
    j["iconPath"]   = util::ToUtf8(d.iconPath);
    j["col"]        = d.col;
    j["row"]        = d.row;
    j["expandStyle"]= util::StyleToName(d.style);
    j["openMode"]   = util::OpenModeToName(d.openMode);
    j["gridCols"]   = d.gridCols;
    json list = json::array();
    for (auto& e : d.shortcuts)
        list.push_back({ {"name", util::ToUtf8(e.name)},
                         {"path", util::ToUtf8(e.path)},
                         {"type", e.type},
                         {"sourcePath", util::ToUtf8(e.sourcePath)},
                         {"mode", util::ShortcutModeToName(e.mode)} });
    j["shortcutList"] = std::move(list);
    return j;
}

void LoadOne(const json& j) {
    if (!j.is_object()) return;
    ContainerData d;
    if (j.contains("id") && j["id"].is_string())
        d.id = j["id"].get<std::string>();
    else
        d.id = "c" + std::to_string(g_containers.size() + 1);
    d.name     = j.contains("name") && j["name"].is_string()
                 ? util::FromUtf8(j["name"].get<std::string>()) : L"容器";
    d.iconPath = AsW(j, "iconPath");
    d.col      = j.value("col", 0);
    d.row      = j.value("row", 0);
    d.style    = util::StyleFromName(j.value("expandStyle", std::string("Grid")));
    d.openMode = util::OpenModeFromName(j.value("openMode", std::string("Click")));
    d.gridCols = j.value("gridCols", 5);
    if (j.contains("shortcutList") && j["shortcutList"].is_array()) {
        for (auto& je : j["shortcutList"]) {
            ShortcutEntry e;
            e.name = AsW(je, "name");
            e.path = AsW(je, "path");
            e.type = je.value("type", std::string());
            e.sourcePath = AsW(je, "sourcePath");
            if (e.path.empty()) continue;
            if (e.type.empty()) e.type = util::GuessType(e.path);
            // 旧配置没有记录原始来源时，按“来自桌面”处理
            if (e.sourcePath.empty()) {
                std::wstring desk = desktop::DesktopFolder();
                if (e.type == "lnk") e.sourcePath = desk + L"\\" + e.name + L".lnk";
                else if (e.type == "folder") e.sourcePath = desk + L"\\" + e.name;
                else e.sourcePath = desk + L"\\" + e.name + L".exe";
            }
            e.mode = util::ShortcutModeFromName(je.value("mode", std::string("Original")));
            d.shortcuts.push_back(std::move(e));
        }
    }
    auto c = std::make_unique<ContainerWindow>(d);
    if (!c->Hwnd()) return;
    g_containers.push_back(std::move(c));
}

} // namespace

namespace Config {

const std::wstring& Path() {
    static const std::wstring p = Dir() + L"/config.json";
    return p;
}
const std::wstring& BackupPath() {
    static const std::wstring b = Dir() + L"/config.backup.json";
    return b;
}

bool SaveNow() {
    json j;
    j["version"] = 2;
    json arr = json::array();
    for (auto& c : g_containers) arr.push_back(SaveOne(*c));
    j["containers"] = std::move(arr);
    j["settings"] = {
        { "maxChars",     g_settings.maxChars },
        { "maxLines",     g_settings.maxLines },
        { "outerScale",   g_settings.outerScale },
        { "innerScale",   g_settings.innerScale },
        { "shortcutMode", util::ShortcutModeToName(g_settings.shortcutMode) }
    };
    std::string txt = j.dump(2);

    CreateDirectoryW(Dir().c_str(), nullptr);
    std::wstring path = Path();
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, txt.data(), (DWORD)txt.size(), &written, nullptr);
    CloseHandle(h);
    if (!ok || written != (DWORD)txt.size()) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str()); return false;
    }
    return true;
}

void MarkDirty() {
    if (g_owner) SetTimer(g_owner, 3, 800, nullptr);   // 防抖：800ms 后落盘
}

bool LoadApp() {
    std::ifstream f(Path(), std::ios::binary);
    if (!f) return false;
    std::string data((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    json j = json::parse(data, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    if (j.contains("settings") && j["settings"].is_object()) {
        const auto& s = j["settings"];
        g_settings.maxChars   = s.value("maxChars", 6);
        g_settings.maxLines   = s.value("maxLines", 3);
        g_settings.outerScale = s.value("outerScale", 1.0);
        g_settings.innerScale = s.value("innerScale", 1.0);
        g_settings.shortcutMode = util::ShortcutModeFromName(
            s.value("shortcutMode", std::string("Original")));
    }
    if (j.contains("containers") && j["containers"].is_array())
        for (auto& je : j["containers"]) LoadOne(je);
    return true;
}

void RemoveAll() {
    // 备份
    CopyFileW(Path().c_str(), BackupPath().c_str(), FALSE);
    // 快捷方式放回桌面
    for (auto& c : g_containers)
        for (auto& e : c->Data().shortcuts) {
            std::wstring target = e.path;
            if (e.type == "lnk") {
                std::wstring t = desktop::LnkTarget(e.path);
                if (!t.empty()) target = t;
            }
            desktop::CreateDesktopShortcut(e.name, target);
        }
    g_containers.clear();          // 释放并销毁窗口
    SaveNow();
}

} // namespace Config
