# DeskStack

纯 **Win32 / WinAPI + C++17** 的桌面折叠容器工具。

> 当前版本：原始稳定版（`issues.md` 中记录的问题暂未在本版修改）
> 问题记录：`issues.md`

## 特性

- 多悬浮容器，作为桌面图标列表 `SysListView32` 的 **`WS_CHILD`** 子窗口
  - 因此 **Win+D 不隐藏**（桌面树的一部分，非外来顶层窗口）
  - 不置顶，可被普通窗口覆盖
  - 桌面图标网格对齐（拖拽松手吸附，按 `LVM_GETITEMSPACING` 单元格取整）
- 5 种展开面板自绘渲染：**网格 / 单列 / 单行 / 扇形 / 环形**
  - 双击面板条目启动 exe / 打开文件夹 / 运行 lnk
  - 失焦 / 点击外部 / Esc 自动关闭
- 图标解析：exe 内嵌图标 / `.lnk` / 文件夹 / ico / 图片，失败用默认占位
- 拖拽收纳：从桌面/资源管理器把 `.lnk/.exe/文件夹` 拖入容器；桌面 `.lnk` 拖入时复制到应用库目录并从桌面移除
- JSON 持久化：`%AppData%/DeskStack/config.json`，保存 **网格索引 (col,row)** 而非像素坐标，DPI 变化自动重算；移除所有容器前备份为 `config.backup.json`
- 系统托盘（中文菜单）：管理界面 / 新建容器 / 恢复全部悬浮容器 / 移除所有容器 / 打开配置文件位置 / 退出
- 容器右键中文菜单：修改名称 / 展开样式 / 删除本容器 / 打开管理界面
- 删除 / 移除容器时，快捷方式自动放回桌面（生成/恢复 `.lnk`）
- Explorer 重启重挂与 Z 序修复（300ms 轮询 + `TaskbarCreated`)
- Per-Monitor V2 高 DPI 适配（合并 manifest）

## 构建

需要 VS2022（MSVC）+ CMake。项目内已内嵌 `nlohmann/json` 单头。

```bat
build.bat
rem 产物：build\DeskStack.exe
```

或手动：

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd build
cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
nmake
```

## 配置格式

`%AppData%/DeskStack/config.json`：

```json
{
  "version": 2,
  "containers": [
    {
      "id": "c1",
      "name": "开发工具",
      "iconPath": "",
      "col": 1, "row": 2,
      "expandStyle": "Grid",
      "gridCols": 5,
      "shortcutList": [
        { "name": "软件A", "path": "C:/x/a.exe", "type": "exe" }
      ]
    }
  ]
}
```

`expandStyle` 内部存英文枚举（Grid/Column/Row/Fan/Ring）

## 源码结构

| 文件 | 职责 |
|---|---|
| `main.cpp` | 入口、托盘宿主、Explorer 重启/轮询、新建/删除容器 |
| `container.cpp` | 悬浮容器：WS_CHILD 创建、拖拽吸附、右键菜单、拖入收纳 |
| `panel.cpp` | 5 种展开样式自绘面板、双击启动、失焦关闭 |
| `layered.cpp` | 32bpp DIB + GDI+ + `UpdateLayeredWindow` 每像素透明渲染 |
| `desktop.cpp` | SysListView32 宿主查找、网格度量、快捷方式还原 |
| `iconlib.cpp` | exe/lnk/文件夹 图标提取 |
| `config.cpp` | JSON 持久化 + 备份 |
| `manager.cpp` | 中文管理界面 |
| `uinp.cpp` | 中文输入对话框 |
| `app.rc` / `resource.h` / `app.ico` | 应用图标资源 |
| `app.manifest` | Per-Monitor V2 DPI 与 ComCtl v6 清单 |
| `issues.md` | 已知问题记录（修改方向） |
