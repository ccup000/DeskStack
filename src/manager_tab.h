#pragma once
#include <windows.h>
#include <string>
#include <vector>

// 新版管理界面：Win11 风格“左侧 tab + 右侧 panel”布局。
// 保留旧版 manager.cpp 不动，新界面统一走这里。
class ManageTab {
public:
    static void Open(HWND owner);

private:
    ManageTab(HWND owner);
    ~ManageTab();

    // 窗口过程
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK NavPaneProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK RightPaneProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK PanelProc(HWND, UINT, WPARAM, LPARAM);

    void OnCreate();
    void OnSize();
    void OnSelectTab(int index);
    void RefreshRightPane();
    void LayoutPanels();
    void UpdateScroll();
    void ScrollBy(int delta);
    void ScrollTo(int pos);
    void OnPanelCommand(HWND panel, int id, WORD code);
    void OnHScroll(HWND track, WORD code);
    void OnNameChanged(HWND panel);
    void OnAddContainer();
    void OnDeleteContainer();
    void OnToggleSetting(int index);

    // 左侧导航
    int  NavHitTest(POINT pt) const;
    void InvalidateNav() const;
    void DrawNav(HDC hdc, const RECT& rc);
    void DrawNavItem(HDC hdc, const RECT& rc, const wchar_t* text, bool selected, bool hover, bool accent, HICON icon = nullptr);
    void DrawOpenModeSwitch(HDC hdc, const RECT& rc, bool rightActive) const;
    void DrawShortcutModeSwitch(HDC hdc, const RECT& rc, bool original) const;
    void DrawShortcutLabel(HDC hdc, const DRAWITEMSTRUCT* dis) const;
    void DrawDeleteButton(HDC hdc, const RECT& rc) const;
    void LoadNavIcons();

    // 右侧 panel 创建
    HWND CreatePanel(const std::wstring& title);
    void AddContainerPanel();
    void AddSettingsPanel();
    void ResizePanelChildren(HWND panel, int width);

    // 控件辅助
    HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id);
    HWND MakeButton(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id, bool enabled = true);
    HWND MakeEdit(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id);
    HWND MakeDeleteButton(HWND parent, int x, int y, int w, int h, int id);
    HWND MakeCombo(HWND parent, int x, int y, int w, int h, int id);
    HWND MakeSwitch(HWND parent, int x, int y, int w, int h, int id);
    HWND MakeShortcutLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id);
    HWND MakeTrackbar(HWND parent, int x, int y, int w, int h, int id, int min, int max, int pos);
    HWND MakeIconPreview(HWND parent, int x, int y, int id);
    void LoadPreviewIcon();

    std::wstring ExePath() const;

    // 状态
    HWND m_hwnd = nullptr;
    HWND m_nav = nullptr;
    HWND m_right = nullptr;
    HWND m_owner = nullptr;

    std::vector<HWND> m_panels;
    std::vector<int>  m_panelHeights;
    HWND m_containerPanel = nullptr;
    std::vector<RECT> m_sectionRects;
    std::vector<HICON> m_navIcons;
    bool m_settingExpanded[4] = { false, false, false, false };
    bool m_originalExpanded = false;
    int m_openModeState = 1;   // 0=悬停，1=点击
    int m_selected = 0;          // 0..N-1 容器；N 表示“设置”
    int m_navHover = -1;
    int m_scrollPos = 0;
    int m_contentH = 0;

    // 视觉资源
    HFONT m_fontNavTitle = nullptr;
    HFONT m_fontNavItem = nullptr;
    HFONT m_fontPanelTitle = nullptr;
    HFONT m_fontText = nullptr;
    HFONT m_fontSmall = nullptr;
    HBRUSH m_whiteBrush = nullptr;
    HBRUSH m_navBrush = nullptr;
    HBRUSH m_hoverBrush = nullptr;
    HBRUSH m_selBrush = nullptr;
    HICON  m_previewIcon = nullptr;
};

// 兼容用户描述中的 “manage_tab” 命名。
using manage_tab = ManageTab;

// 便捷入口：供 main.cpp 托盘菜单调用。
void OpenManagerTabWindow(HWND owner);
