#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _USE_MATH_DEFINES
#include <windows.h>
#include <vector>
#include "types.h"

namespace Gdiplus { class Graphics; }
class ContainerWindow;

// 点击容器后弹出的快捷面板：5 种展开样式自绘，双击打开，失焦/Esc 关闭。
class PanelWindow {
public:
    PanelWindow(ContainerWindow* owner, const ContainerData& data);
    ~PanelWindow();

    HWND Hwnd() const { return m_hwnd; }
    void Render();
    void Close();
    static void RegisterClass(HINSTANCE hInst);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    // 计算各条目中心（屏幕坐标），并据此设置窗口位置与尺寸
    void ComputeLayout(std::vector<POINT>& centers);
    int  ItemAt(POINT client) const;   // → 索引或 -1
    void DrawItem(Gdiplus::Graphics& g, int idx, int iconSize,
                  const ShortcutEntry& e);
    void Draw(Gdiplus::Graphics& g, int w, int h);
    void DrawPanelBackground(Gdiplus::Graphics& g, int w, int h);

    // 拖动容器内条目到桌面
    void DragStart(int idx, POINT clientPt);
    void DragMove(POINT screenPt);
    void DragEnd(POINT screenPt);      // 在屏幕坐标释放
    void DragCancel();

    HWND m_hwnd = nullptr;
    ContainerWindow* m_owner = nullptr;
    ContainerData m_data;               // 快照：面板打开时的样式与条目
    std::vector<POINT> m_centers;       // 条目图标中心（客户区坐标）
    std::vector<RECT> m_plates;         // 每个条目“实际区域”（图标+文字包围盒），用于布局/命中/hover
    int m_iconSize = 48;                // 逻辑像素
    int m_hoverItem = -1;               // 当前 hover 条目（Windows 式底色）
    // 面板中心（客户区坐标，即触发容器中心）：Fan/Ring 以此为圆心绘制背景形状
    int m_cx = 0, m_cy = 0;
    // 扇形/环形最终使用的屏幕圆心（可能因屏幕边界而相对容器中心平移）
    int m_screenCenterX = 0, m_screenCenterY = 0;
    int m_radius = 0;      // Fan/Ring 图标中心半径
    int m_rout = 0, m_rin = 0; // 外/内半径
    double m_aDeg = 0;     // Fan/Ring 展开角（度），与背景绘制一致
    int m_labelW = 0;      // 条目文字绘制宽度（受限于单元格，防溢出重叠）
    int m_windowX = 0, m_windowY = 0;   // 窗口屏幕位置
    int m_windowW = 0, m_windowH = 0;   // 窗口客户端尺寸

    // 拖出到桌面状态
    bool   m_draggingOut = false;
    bool   m_inDragDrop = false;    // DragEnd 临时隐藏面板时，避免 WA_INACTIVE 误关
    int    m_dragIdx = -1;
    POINT  m_downPt{};
    HWND   m_dragWnd = nullptr;
};
