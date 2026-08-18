#pragma once
#include "types.h"
#include <vector>
#include <memory>
#include <windows.h>

class ContainerWindow;
class PanelWindow;

// 跨模块消息（发给托盘宿主 g_owner）
#define WM_CONTAINER_DELETE  (WM_APP + 50)   // wp=容器 HWND，请求删除
#define WM_OPEN_MANAGER      (WM_APP + 51)   // 打开管理界面
#define WM_CONTAINER_NEW     (WM_APP + 52)   // 新建容器
#define WM_SHOW_ALL          (WM_APP + 53)   // 恢复全部悬浮容器
#define WM_REMOVE_ALL        (WM_APP + 54)   // 移除所有容器

// 全局应用状态
extern std::vector<std::unique_ptr<ContainerWindow>> g_containers;
extern HWND g_owner;                       // 托盘宿主窗口

// 由 config.cpp 实现
namespace Config { void MarkDirty(); bool SaveNow(); }
