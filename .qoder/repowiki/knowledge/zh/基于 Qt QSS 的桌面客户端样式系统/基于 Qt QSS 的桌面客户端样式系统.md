---
kind: frontend_style
name: 基于 Qt QSS 的桌面客户端样式系统
category: frontend_style
scope:
    - '**'
source_files:
    - client/llfcchat/resources/style/stylesheet.qss
    - client/llfcchat/src/main.cpp
    - client/llfcchat/resources/rc.qrc
    - client/llfcchat/ui/mainwindow.ui
    - client/llfcchat/ui/chatdialog.ui
    - client/llfcchat/ui/logindialog.ui
    - client/llfcchat/ui/registerdialog.ui
    - client/llfcchat/ui/applyfriendpage.ui
    - client/llfcchat/include/BubbleFrame.h
    - client/llfcchat/include/TextBubble.h
    - client/llfcchat/include/PictureBubble.h
    - client/llfcchat/include/FileBubble.h
---

## 1. 使用的系统与工具

本仓库的前端（即桌面客户端）基于 **Qt** 构建，采用 **Qt StyleSheet (QSS)** 作为唯一的 UI 样式方案。样式集中存放在 `client/llfcchat/resources/style/stylesheet.qss`，通过 `main.cpp` 在应用启动时加载并全局应用到 `QApplication`：

```cpp
QFile qss(":/style/stylesheet.qss");
... a.setStyleSheet(style);
```

资源文件通过 Qt 资源系统（`.qrc`）打包，图标、图片等资源路径统一使用 `:/res/xxx.png` 形式引用。

## 2. 关键文件

- `client/llfcchat/resources/style/stylesheet.qss` — 全部视觉样式定义（约 800 行），覆盖登录/注册/聊天/好友申请/设置等所有界面。
- `client/llfcchat/src/main.cpp` — 应用入口，负责读取并应用 QSS。
- `client/llfcchat/ui/*.ui` — Qt Designer 生成的界面布局文件，与 QSS 配合工作。
- `client/llfcchat/resources/res/*.png` — 按钮、图标、气泡提示等位图资源。
- `client/llfcchat/include/BubbleFrame.h`, `TextBubble.h`, `PictureBubble.h`, `FileBubble.h` — 消息气泡组件，部分在 C++ 中通过 `setStyleSheet()` 局部覆盖样式。
- `client/llfcchat/CMakeLists.txt` — 将 `.qrc` 资源纳入构建。

## 3. 架构与约定

### 3.1 样式组织方式
- **单一 QSS 文件**：所有样式集中在 `stylesheet.qss`，没有按页面或组件拆分多个样式文件。
- **ID 选择器为主**：大量使用 `#id_name[state='...']` 形式的 ID 选择器，结合自定义 `state` 属性实现按钮/标签的多状态外观（normal/hover/press/selected_normal/selected_hover/selected_pressed/unvisible/visible 等）。
- **类型选择器 + 伪类**：对通用控件如 `QScrollBar`、`QListWidget::item:selected`、`QLabel` 等使用标准伪类进行全局美化。
- **字体统一**：几乎所有文本元素显式指定 `font-family: "Microsoft YaHei"`，字号在 11px~18px 之间分级。

### 3.2 主题色与视觉规范
- **主色调**：绿色系（`#07c160` / `#2cb46e` / `#3bbb4b` / `#48bf56`）用于确认/发送/成功态按钮；灰色系（`#f0f0f0` / `#d3d7d4` / `#eaeaea` / `#f7f7f8`）用于背景与禁用态。
- **侧边栏**：深色背景 `rgb(46,46,46)`，与浅色聊天区形成对比。
- **圆角按钮**：常用按钮统一 `border-radius: 20px`，营造柔和风格。
- **滚动条**：自定义垂直滚动条，宽度 8px，滑块圆角 4px，轨道透明。

### 3.3 资源管理
- 所有图片通过 Qt 资源系统以 `:/res/xxx.png` 引用，避免硬编码绝对路径。
- 图标资源命名遵循 `<功能>_<状态>.png` 模式（如 `add_friend_normal.png` / `add_friend_hover.png` / `add_friend_press.png`），与 QSS 中的 `state` 选择器一一对应。

### 3.4 运行时样式覆盖
部分组件在 C++ 代码中通过 `setStyleSheet()` 动态设置局部样式（如 `TextBubble` 中隐藏 QTextEdit 边框、`PictureBubble` 中设置进度条样式、`FileBubble` 中设置文件大小标签颜色），属于对全局 QSS 的细粒度补充。

## 4. 约定与约束

- **样式来源唯一**：全局样式必须来自 `stylesheet.qss`，由 `main.cpp` 统一加载；新增 UI 应优先通过修改 QSS 而非在 C++ 中写 `setStyleSheet`。
- **状态驱动外观**：交互态（hover/press/selected）通过 `state` 属性 + QSS 选择器控制，而非依赖 Qt 内置 hover 行为——这意味着每个可交互控件需在 UI 文件中声明对应 `state` 属性并在逻辑中切换。
- **资源路径规范**：图片一律使用 `:/res/xxx.png` 形式，禁止使用相对路径或绝对路径。
- **字体强制**：所有可见文本需显式指定 `Microsoft YaHei` 字体，保证中文渲染一致。
- **无 CSS/SCSS/Tailwind**：本项目不使用 Web 前端技术栈，不存在 CSS 文件、CSS-in-JS、Tailwind 或其他现代前端样式工具。
- **无设计令牌抽象层**：颜色、字号等值直接写在 QSS 中，未抽取为变量或常量文件，因此缺乏集中化的设计令牌管理。

## 5. 置信度

**high**：存在完整的 QSS 样式文件、统一的加载机制、一致的命名与状态约定，证据充分且贯穿整个客户端 UI。