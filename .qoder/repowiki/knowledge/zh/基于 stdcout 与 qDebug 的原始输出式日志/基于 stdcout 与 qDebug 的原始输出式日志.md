---
kind: logging_system
name: 基于 std::cout 与 qDebug 的原始输出式日志
category: logging_system
scope:
    - '**'
source_files:
    - server/ChatServer/src/CSession.cpp
    - client/llfcchat/include/tcpmgr.h
    - server/ChatServer/include/MysqlDao.h
---

该仓库未实现统一的日志系统，所有模块均使用最基础的原始输出来进行调试和错误记录：

- **服务端（ChatServer/GateServer/ResourceServer/StatusServer）**：在 C++ 代码中广泛使用 `std::cout` / `std::endl` 直接打印到标准输出，例如 `CSession.cpp` 中的网络读写错误、消息长度校验异常、会话销毁等；`MysqlDao.h` 中 MySQL 连接初始化与重连状态也通过 `std::cout` 输出。没有引入 spdlog、log4cplus、fmt 等任何第三方日志库。
- **客户端（Qt 桌面端）**：主要使用 Qt 自带的 `qDebug()` 进行调试输出，散落在 `applyfriend.cpp`、`authenfriend.cpp`、`tcpmgr.h`、`ChatView.cpp` 等文件中，用于 UI 交互流程跟踪和线程信息打印。
- **Node.js 验证服务**：作为独立的后端服务，未见专门的日志框架配置。

**关键特征**：
1. 无集中式日志初始化或配置文件；
2. 无结构化日志字段（如时间戳、级别、模块名）；
3. 无日志级别管理（info/debug/warn/error）；
4. 无日志文件输出或异步写入机制；
5. 所有输出均为同步控制台打印，不适合生产环境。

这种“裸输出”方式仅适用于开发调试阶段，不具备可观测性、可检索性和性能隔离能力。