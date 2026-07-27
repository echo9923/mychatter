---
kind: external_dependency
name: Qt5 GUI 框架
slug: qt5
category: external_dependency
category_hints:
    - framework_behavior
scope:
    - '**'
source_files:
    - client/llfcchat/src/main.cpp
    - client/llfcchat/include/mainwindow.h
---

客户端使用 Qt5 构建桌面应用程序，包含 UI 界面、网络通信、文件处理等功能。主要组件包括 QMainWindow、QDialog、QTableView、QTextEdit 等控件，使用 QSS 样式表美化界面。Qt 的信号槽机制用于事件处理，QNetworkAccessManager 处理 HTTP 请求，自定义 TCP 管理器处理长连接。支持 WebP 图片格式显示。