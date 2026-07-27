---
kind: frontend_style
name: Qt 客户端 QSS 样式系统与 WebRTC 演示页面
category: frontend_style
scope:
    - '**'
source_files:
    - client/llfcchat/resources/style/stylesheet.qss
    - webrtc-demo/web/index.html
---

## 前端样式系统概述

本项目包含两个独立的前端样式实现：基于 Qt 的桌面客户端使用 QSS（Qt Style Sheets）进行 UI 样式管理，以及一个独立的 WebRTC 1v1 视频通话演示页面。

### Qt 客户端样式系统

**核心文件**: `client/llfcchat/resources/style/stylesheet.qss`

项目采用集中式的 QSS 样式管理策略，所有样式定义集中在单个 stylesheet.qss 文件中，通过 Qt 的资源系统加载。样式系统具有以下特点：

- **命名空间组织**: 使用 `#` 选择器配合组件 ID 进行样式隔离，如 `#LoginDialog`、`#RegisterDialog`、`#side_bar`、`#chat_user_list` 等
- **状态驱动样式**: 广泛使用 `[state='xxx']` 伪类控制组件不同状态，包括 `normal`、`hover`、`press`、`selected_normal`、`selected_hover` 等
- **资源引用**: 通过 `:/res/` 前缀引用 Qt 资源系统中的图片资源，如头像、图标、背景图等
- **主题色彩体系**: 采用统一的配色方案，主色调为绿色系 (`#2cb46e`、`#07c160`)，辅助色为灰色系 (`#f0f0f0`、`#d3d7d4`)，背景色以浅灰为主

**样式覆盖范围**:
- 对话框样式: LoginDialog、RegisterDialog、ResetDialog、LoadingDlg
- 导航组件: side_bar、search_wid、tool_wid
- 列表组件: chat_user_list、con_user_list、search_list
- 按钮组件: send_btn、receive_btn、add_friend_btn、sure_btn、cancel_btn
- 输入控件: chatEdit、ApplyFriend、name_ed、back_ed
- 消息气泡: msg_chat、video_chat、voice_chat
- 自定义滚动条: QScrollBar 垂直滚动条定制

### WebRTC 演示页面样式

**核心文件**: `webrtc-demo/web/index.html`

WebRTC 演示页面采用极简的内联 CSS 样式设计，主要特点：

- **响应式布局**: 使用 CSS Grid 和 Flexbox 实现自适应布局
- **现代化字体**: 使用 `system-ui, sans-serif` 字体栈
- **简洁视觉**: 基础的颜色方案和圆角设计
- **内联样式**: 所有样式直接嵌入 HTML 文件的 `<style>` 标签中

### 设计约定与规范

1. **颜色规范**: 
   - 主操作色: `#2cb46e` (绿色)
   - 背景色: `#f0f0f0`、`#ffffff`、`#f7f7f8`
   - 文字色: `#161616` (深色)、`#153153153153` (灰色)
   - 边框色: `#f1f1f1`、`#ede9e7`

2. **字体规范**: 统一使用 "Microsoft YaHei" 字体，字号从 12px 到 18px 不等

3. **交互状态**: 所有可交互元素都定义了 normal/hover/press 三种状态的样式

4. **资源管理**: 所有图片资源通过 Qt 资源系统管理，使用 `:/res/` 路径前缀

### 开发建议

- 新增组件时应遵循现有的命名约定，使用 `#component_id[state='state']` 格式
- 保持颜色一致性，优先使用已定义的配色方案
- 对于复杂组件，考虑将样式拆分为独立的 QSS 文件以提高可维护性
- WebRTC 演示页面应保持简洁，专注于功能演示而非样式美化