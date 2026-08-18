#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include "types.h"

class PanelWindow;

// 悬浮容器窗口：SysListView32 的子窗口（WS_CHILD + WS_EX_LAYERED）。
// 显示容器图标；单击弹出面板；拖拽移动并按桌面网格吸附；右键中文菜单。
class ContainerWindow {
public:
    ContainerWindow(const ContainerData& data);
    ~ContainerWindow();

    HWND Hwnd() const { return m_hwnd; }
    ContainerData& Data() { return m_data; }
    const ContainerData& Data() const { return m_data; }

    void Render();                 // 重绘（UpdateLayeredWindow）
    void ReapplyPosition();        // 按网格索引重新定位（DPI/Explorer 重启后）

    // 把当前网格索引写入 m_data 并保存
    void SetGridIndex(int col, int row);

    static void RegisterClass(HINSTANCE hInst);
    void CloseActivePanel();          // 关闭本容器已打开的面板
    void AddShortcut(const std::wstring& srcPath);   // 拖入/添加一个快捷方式条目
    void SetDragOver(bool over);              // 拖放悬停高亮
    void SetIconPath(const std::wstring& path);  // 设置容器自身图标（ico/exe）

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    bool HitDraggable(POINT pt) const;       // 命中即允许拖动整个容器
    void OpenPanel();

    HWND m_hwnd = nullptr;
    ContainerData m_data;

    // 拖拽状态
    bool   m_pressed = false;     // 左键按下（用于区分单击/拖拽）
    bool   m_dragging = false;
    POINT  m_downClient{};
    POINT  m_downStartParent{};   // 拖拽开始时窗口左上角（父客户区坐标）
    int    m_offsetX = 0, m_offsetY = 0;   // 按下点相对窗口左上角

    // 面板
    PanelWindow* m_panel = nullptr;
    bool   m_dragOver = false;      // 拖放悬停
    friend class PanelWindow;
};
