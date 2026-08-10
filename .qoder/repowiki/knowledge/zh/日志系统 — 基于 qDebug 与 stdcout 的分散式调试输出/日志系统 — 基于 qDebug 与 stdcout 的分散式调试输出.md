---
kind: logging_system
name: 日志系统 — 基于 qDebug 与 std::cout 的分散式调试输出
category: logging_system
scope:
    - '**'
source_files:
    - server/ChatServer/src/CServer.cpp
    - client/llfcchat/src/main.cpp
    - client/llfcchat/src/applyfriend.cpp
    - client/llfcchat/src/authenfriend.cpp
    - tests/integration/im_common.h
    - server/common/include/Singleton.h
    - client/llfcchat/include/singleton.h
---

本仓库未引入专门的日志框架（如 spdlog、log4cplus、Boost.Log 等），也没有统一的日志模块或集中式日志配置。代码中的“日志”行为完全由开发者在业务文件中直接调用 Qt 的 `qDebug()` 以及 C++ 标准库的 `std::cout`/`printf` 完成，属于**分散式、无结构化、无级别管理**的调试输出。

### 1. 使用的系统与工具
- **客户端（Qt）**：大量使用 `qDebug() << ...` 进行调试输出，部分位置被注释掉（如 `MessageTextEdit.cpp`、`applyfriendpage.cpp` 中多处 `//qDebug(...)`），说明开发过程中频繁开关调试信息。
- **服务端（C++/Asio）**：直接使用 `std::cout << ... << std::endl` 输出启动、错误、定时器异常等信息，例如 `CServer.cpp` 中的 “Server start success”、“session accept failed”、“timer error” 等。
- **测试代码**：`tests/integration/im_common.h` 中自定义了 `LOG_INFO` 宏，内部通过 `std::vprintf` + `std::printf` 输出 `[PASS]` 等测试结果。
- **单例模板**：`server/common/include/Singleton.h` 和 `client/llfcchat/include/singleton.h` 的析构函数中也包含 `std::cout` 输出，用于跟踪对象生命周期。

### 2. 关键文件与位置
- `server/ChatServer/src/CServer.cpp`：服务端核心启动、会话接受、定时器异常均通过 `std::cout` 输出。
- `client/llfcchat/src/main.cpp`：Qt 应用入口，使用 `qDebug("open success")` 打印样式加载结果。
- `client/llfcchat/src/applyfriend.cpp`、`authenfriend.cpp`、`ChatView.cpp`：UI 交互逻辑中使用大量 `qDebug()` 打印调试信息。
- `tests/integration/im_common.h`：测试框架中的 `LOG_INFO` 宏封装了 `printf` 输出。
- `server/common/include/Singleton.h`、`client/llfcchat/include/singleton.h`：单例析构时输出地址与销毁信息。

### 3. 架构与约定
- **无统一日志抽象层**：各模块直接依赖 `qDebug` 或 `std::cout`，没有封装统一的 Logger 类或宏。
- **无日志级别**：所有输出均为调试级别，无法区分 INFO/WARN/ERROR，也无法按级别过滤。
- **无结构化格式**：输出为纯文本拼接，不包含时间戳、线程 ID、模块名、请求 ID 等结构化字段。
- **无集中配置**：没有配置文件控制日志级别、输出目标（控制台/文件/网络）、格式化规则等。
- **无异步写入**：所有输出均为同步阻塞 I/O，在高并发场景下可能影响性能。

### 4. 约定与约束
- **观察到的模式**：
  - 客户端 UI 相关逻辑优先使用 `qDebug()`，便于在 Qt Creator 输出窗口查看。
  - 服务端 Asio 回调与错误处理路径使用 `std::cout`，保持与 Boost.Asio 风格一致。
  - 测试代码使用自定义 `LOG_INFO` 宏，仅用于单元测试输出。
- **非强制约束**：未发现任何编译期检查、lint 规则或 CI 流程强制规范日志使用方式；开发者可自由选择在任意位置插入 `qDebug`/`std::cout`。
- **潜在问题**：由于缺乏统一日志系统，生产环境难以收集、分析、告警；调试信息散落在各处，不利于问题定位。

总结：该项目的日志系统处于**原始阶段**，仅满足开发调试需求，未形成工程化的日志体系。若需改进，建议引入结构化日志框架（如 spdlog）并建立统一的日志抽象层。