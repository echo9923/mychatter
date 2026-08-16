---
kind: frontend_style
name: Qt 客户端样式系统（QSS + 资源文件）
category: frontend_style
scope:
    - '**'
source_files:
    - client/llfcchat/resources/style/stylesheet.qss
    - client/llfcchat/resources/rc.qrc
    - client/llfcchat/src/main.cpp
    - client/llfcchat/src/mainwindow.cpp
---

本项目的 UI 样式完全基于 Qt 的 QSS（Qt Style Sheets）机制，配合 Qt 资源系统（.qrc）统一管理静态资源，形成一套集中式、无框架依赖的桌面端视觉风格体系。

**样式系统与工具链**
- 样式语言：QSS（Qt Style Sheets），语法与 CSS 高度相似，用于定义控件外观、颜色、字体、边框、背景等。
- 资源管理：通过 `resources/rc.qrc` 将所有图片、动画和样式表打包进可执行文件，使用 `:/res/...` 和 `:/style/stylesheet.qss` 路径引用。
- 样式加载：在 `src/main.cpp` 中启动时一次性读取并应用全局样式表，所有子控件继承该样式。

**核心文件与组织方式**
- 样式主文件：`client/llfcchat/resources/style/stylesheet.qss`（815 行），按控件 ID（如 `#add_btn`、`#send_btn`、`#side_chat_lb`）和类名（如 `ApplyFriendItem`、`LoadingDlg`）分组定义。
- 资源清单：`client/llfcchat/resources/rc.qrc`，集中声明所有被 QSS 引用的图片资源（头像、按钮状态图、图标等）。
- 样式注入点：`src/main.cpp` 中通过 `QFile(":/style/stylesheet.qss")` 加载样式并调用 `a.setStyleSheet(style)` 应用到整个应用。

**架构与设计约定**
- **ID 选择器优先**：绝大多数样式通过 `#控件名[state='xxx']` 形式绑定到具体控件实例，而非通用类选择器，确保样式隔离且可预测。
- **状态驱动外观**：按钮、标签等交互元素统一使用 `state` 属性区分 `normal`、`hover`、`press`、`selected_normal`、`selected_hover` 等状态，每种状态对应不同的 `border-image` 或背景色。
- **图片化 UI**：大量使用 `border-image` 加载 PNG 切片图实现圆角按钮、图标切换等效果，避免纯代码绘制带来的维护成本。
- **字体与色彩规范**：全局统一使用 `Microsoft YaHei` 字体；主色调为绿色系（`#2cb46e`、`#07c160`、`#3bbb4b`），辅助灰度色系（`#f0f0f0`、`#d3d7d4`、`#eaeaea`）用于背景和分隔线。
- **滚动条定制**：通过 `QScrollBar:vertical` 自定义滚动条宽度（8px）、滑块颜色（`rgb(173,170,169)`）和圆角（4px），移除默认箭头按钮。

**约束与一致性规则**
- 所有对话框均设置 `Qt::CustomizeWindowHint|Qt::FramelessWindowHint` 窗口标志，实现无边框统一外观（见 `mainwindow.cpp` 中各 Dialog 创建逻辑）。
- 列表控件（`QListWidget`、`QTreeWidget`）统一隐藏边框和焦点轮廓（`border: none; outline: none;`），并通过 `::item:selected` 和 `::item:hover` 定义选中/悬停态。
- 输入框（`QLineEdit`、`QTextEdit`）普遍设置 `border: none` 或极细边框，保持极简风格。
- 按钮统一采用圆角（`border-radius: 20px`）+ 内边距 + 固定字号（14px/16px/18px）的三段式结构。
- 部分组件通过运行时 `setStyleSheet()` 覆盖默认样式（如 `TextBubble.cpp` 中 `QTextEdit{background:transparent;border:none}`），作为 QSS 的补充手段。