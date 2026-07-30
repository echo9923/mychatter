---
kind: frontend_style
name: Qt 客户端样式系统（QSS 主题与资源管理）
category: frontend_style
scope:
    - '**'
source_files:
    - client/llfcchat/resources/style/stylesheet.qss
    - client/llfcchat/src/main.cpp
    - client/llfcchat/resources/rc.qrc
---

本项目前端样式完全基于 Qt 的 QSS（Qt Style Sheets）机制，采用单一全局样式文件集中管理所有 UI 外观，配合 Qt 资源系统（rc.qrc）统一管理图标、图片等静态资源。

**样式系统与架构**
- 核心样式文件：`client/llfcchat/resources/style/stylesheet.qss`（815 行），通过 `main.cpp` 在应用启动时加载并全局设置
- 资源管理：`client/llfcchat/resources/rc.qrc` 将样式文件与大量 PNG/JPG/GIF 图标打包进可执行文件，使用 `:/res/` 和 `:/style/` 前缀引用
- 无 CSS/SCSS/Tailwind 等 Web 技术栈，纯 Qt 原生样式方案

**样式组织模式**
- 按组件 ID 命名：大量使用 `#id_name[state='state']` 语法定义按钮、标签等控件的不同状态（normal/hover/pressed/selected_*）
- 统一字体规范：全局使用 "Microsoft YaHei" 字体，字号集中在 12px-18px 范围
- 颜色体系：主色调为绿色系（#2cb46e、#07c160、#3bbb4b），背景色以浅灰（#f7f7f8、#eaeaea）为主，深色侧边栏（rgb(46,46,46)）
- 交互状态：通过自定义 `state` 属性控制控件外观变化，而非依赖 Qt 内置状态

**资源与主题约定**
- 图标资源：`resources/res/` 目录下按功能分类存放（chat_icon、settings、add_friend 等），每个图标提供 normal/hover/press 三态
- 头像资源：head_1.jpg ~ head_5.jpg 作为默认用户头像
- 滚动条定制：全局重写 QScrollBar 样式，实现圆角滑块和无边框设计
- 对话框主题：LoginDialog、RegisterDialog、ResetDialog、LoadingDlg 等独立对话框有专属背景色

**开发约束**
- 新增控件必须定义对应 state 状态的样式规则
- 图标资源需遵循三态命名规范（*_normal.png、*_hover.png、*_press.png）
- 避免在代码中硬编码样式，应优先使用 QSS 文件管理
- 保持字体、颜色、圆角半径等视觉元素的一致性