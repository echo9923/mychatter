---
kind: logging_system
name: 基于 std::cout / qDebug 的原始日志输出（无结构化日志框架）
category: logging_system
scope:
    - '**'
source_files:
    - server/ChatServer/src/CServer.cpp
    - server/GateServer/src/CServer.cpp
    - client/llfcchat/src/main.cpp
    - client/llfcchat/src/applyfriend.cpp
    - client/llfcchat/src/authenfriend.cpp
    - client/llfcchat/src/ChatView.cpp
    - client/llfcchat/include/singleton.h
    - vcpkg.json
---

## 1. 使用的系统/方案

仓库中**没有引入任何第三方日志库**（vcpkg.json 仅包含 boost-asio、grpc、protobuf、nlohmann-json、hiredis、openssl、mysql-connector-cpp、qt5-base、qt5-imageformats，未包含 spdlog/log4cpp 等）。服务端与客户端均使用最原始的 C++ 标准输出进行调试/运行期信息输出：

- **服务端（ChatServer/GateServer/ResourceServer/StatusServer）**：直接使用 `std::cout` / `std::cerr` 配合 `#include <iostream>` 打印启动、监听端口、异常、定时器错误等信息。
- **Qt 客户端（client/llfcchat）**：使用 Qt 的 `qDebug()` 宏输出 UI 调试信息（如窗口尺寸、信号槽调用、文件对话框打开结果等）。

因此，本项目不存在“日志级别管理”、“结构化字段”、“多 sink 路由”或“异步落盘”等能力——所有日志都是同步写入标准输出流。

## 2. 关键文件

| 位置 | 说明 |
|---|---|
| `server/ChatServer/src/CServer.cpp` | 服务端入口类，构造/析构时 `cout << "Server start success..."`，accept 失败和心跳定时器错误通过 `cout` 输出 |
| `server/GateServer/src/CServer.cpp` | Gate 服务 accept 回调中的 `catch (std::exception& exp)` 分支用 `std::cout` 打印异常 |
| `client/llfcchat/src/main.cpp` | Qt 应用启动时 `qDebug("open success")` / `qDebug("Open failed")` 输出 QSS 加载状态 |
| `client/llfcchat/src/applyfriend.cpp`、`authenfriend.cpp`、`ChatView.cpp` | 大量 `qDebug() << ...` 用于 UI 调试（窗口尺寸、信号触发、关闭信号等） |
| `client/llfcchat/include/singleton.h` | 单例模板的析构路径中 `std::cout` 输出销毁信息 |
| `vcpkg.json` | 依赖清单，确认无日志库 |

## 3. 架构与约定

- **无集中式日志模块**：每个服务/组件自行在需要的位置直接调用 `cout` / `qDebug`，没有统一的 logger 单例、日志门面或初始化入口。
- **无日志级别**：所有输出均为同一优先级；无法区分 debug/info/warn/error。
- **无结构化字段**：日志为纯文本拼接字符串，不包含 JSON 结构体、时间戳、进程名、线程 ID、来源文件/行号等元数据。
- **无 sink 配置**：输出目标固定为标准输出（stdout/stderr），无法重定向到文件或网络。
- **阻塞式 I/O**：`cout` / `qDebug` 是同步调用，在高并发连接场景下可能成为瓶颈（当前代码也未做缓冲或异步化）。
- **JSON 仅用于业务数据**：`nlohmann::json` 被广泛用作 gRPC metadata、Redis 序列化、HTTP body 等业务数据的载体，但**不用于日志记录**。

## 4. 约定与约束

- **描述性约定**：服务端在关键生命周期节点（构造函数、析构函数、accept 回调、定时器回调）输出人类可读的启动/停止/错误信息；客户端在 UI 交互关键点（按钮点击、窗口尺寸变化、信号槽触发）输出调试信息。
- **约束来源**：vcpkg.json 明确未声明任何日志库；全仓 grep 未发现 `spdlog`、`log4cpp`、`boost::log`、`fmt::log` 等引用；所有日志输出均散落在业务源文件中，无独立 logging 目录或头文件。
- **可观察到的行为**：运行任一服务端二进制会在 stdout 看到形如 `Server start success, listen on port : xxx` 的纯文本行；Qt 客户端运行时会在控制台看到 `qDebug` 输出的调试行。