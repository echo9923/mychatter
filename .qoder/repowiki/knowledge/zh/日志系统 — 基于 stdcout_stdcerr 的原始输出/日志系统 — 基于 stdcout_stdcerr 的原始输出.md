---
kind: logging_system
name: 日志系统 — 基于 std::cout/std::cerr 的原始输出
category: logging_system
scope:
    - '**'
source_files:
    - server/ChatServer/src/CSession.cpp
    - server/ChatServer/src/CServer.cpp
    - server/ChatServer/src/AsioIOServicePool.cpp
    - server/ChatServer/src/ConfigMgr.cpp
---

该仓库未实现专门的日志框架或结构化日志系统。服务端与客户端代码中广泛使用 C++ 标准库的 `std::cout`、`std::cerr` 以及 `printf`/`fprintf` 进行调试输出，没有任何统一的日志级别管理、日志文件输出、异步写入或结构化字段封装。

具体表现：
- 所有日志均为控制台直接输出，格式为字符串拼接（如 `"session: " << _session_id << " send que fulled, size is " << MAX_SENDQUE << endl`），无统一前缀、时间戳或线程标识。
- 错误信息混在 `std::cout` 和 `std::cerr` 中，没有按严重性分级。
- 未发现任何第三方日志库（如 spdlog、glog、log4cplus）的引入或使用。
- 配置文件（如 `server/*/config/*.ini`）中也没有日志级别、输出目标等配置项。

这种散乱的 `cout/cerr` 输出方式属于开发阶段的临时调试手段，不具备生产环境所需的可观测性、可检索性和性能控制能力。