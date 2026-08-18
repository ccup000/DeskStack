#pragma once
#include <string>

// JSON 持久化：%AppData%/DeskStack/config.json
namespace Config {

const std::wstring& Path();
const std::wstring& BackupPath();

bool SaveNow();              // 立即写盘（原子替换）
void MarkDirty();            // 延迟写盘（由 g_owner 的 timer 3 触发）
bool LoadApp();              // 从配置恢复全部容器；无/坏文件返回 false

// 移除所有容器：先备份 config.backup.json，把快捷方式放回桌面，再清空写入
void RemoveAll();

} // namespace Config
