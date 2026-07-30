---
kind: external_dependency
name: Boost.Asio 网络库
slug: boost-asio
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
---

### Boost.Asio 网络库
- **角色**：项目核心网络 IO 库，用于实现 TCP 长连接、HTTP 服务器、异步 IO 模型
- **集成点**：所有 C++ 服务端（GateServer、ChatServer、StatusServer、ResourceServer）和客户端都使用 Asio 作为网络层基础
- **使用模式**：通过 `AsioIOServicePool` 实现多线程 IO 线程池，每个服务实例维护独立的 `io_context`，采用 round-robin 分配连接
- **关键特性**：支持异步读写、定时器、信号处理，解决 TCP 粘包问题（先读头再读体的两阶段读取）
- **验证**：vcpkg.json 中声明依赖，各服务 main 函数中初始化 io_context
- **注意**：项目未使用独立业务线程池，业务逻辑直接在 IO 线程执行