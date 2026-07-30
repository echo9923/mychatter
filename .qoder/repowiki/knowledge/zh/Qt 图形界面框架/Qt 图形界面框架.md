---
kind: external_dependency
name: Qt 图形界面框架
slug: qt
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
---

### Qt 图形界面框架
- **角色**：客户端 UI 开发框架，提供窗口、控件、事件处理等 GUI 能力
- **集成点**：客户端 llfcchat 项目完全基于 Qt 开发，包含多个对话框和主窗口
- **主要组件**：
  - Qt5 Base：核心 GUI 功能（QWidget、QMainWindow、信号槽机制）
  - Qt5 ImageFormats：图片格式支持（包括 WebP）
  - QNetworkAccessManager：HTTP 请求处理
  - QTcpSocket：TCP 网络连接
- **UI 结构**：登录对话框、聊天主界面、好友管理、图片浏览器等多个界面
- **资源管理**：使用 .qrc 资源文件管理图标、样式表等资源
- **注意**：客户端使用 Qt 自带的 JSON 类（QJsonObject），不依赖第三方 JSON 库