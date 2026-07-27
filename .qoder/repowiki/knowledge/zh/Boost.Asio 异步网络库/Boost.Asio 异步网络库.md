---
kind: external_dependency
name: Boost.Asio 异步网络库
slug: boost-asio
category: external_dependency
category_hints:
    - framework_behavior
scope:
    - '**'
source_files:
    - server/*/include/AsioIOServicePool.h
    - server/*/src/CSession.cpp
---

项目使用 Boost.Asio 作为核心网络库，实现 TCP 长连接、HTTP 服务器（通过 Beast）、异步 IO 处理。各服务端（GateServer、ChatServer、StatusServer、ResourceServer）都基于 AsioIOServicePool 实现多线程 IO 模型，每个服务维护独立的 io_context 线程池，通过 round-robin 分配连接。Asio 负责消息的粘包拆包处理、心跳保活机制、以及所有网络通信的异步回调。