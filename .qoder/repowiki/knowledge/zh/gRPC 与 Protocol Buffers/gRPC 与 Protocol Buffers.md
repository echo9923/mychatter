---
kind: external_dependency
name: gRPC 与 Protocol Buffers
slug: grpc-protobuf
category: external_dependency
category_hints:
    - vendor_identity
    - sdk_real_api
scope:
    - '**'
---

### gRPC 与 Protocol Buffers
- **角色**：服务间通信框架，定义强类型 IDL 接口，提供高性能序列化
- **集成点**：三个 proto 文件定义服务接口（chat_service/chat.proto、status_service/status.proto、verify_service/verify.proto）
- **使用模式**：C++ 服务通过 gRPC 调用 Node.js 的 VerifyServer，以及 ChatServer 之间的跨服通信
- **数据格式**：服务间通信使用 protobuf 二进制格式，性能优于 JSON；对外 HTTP 接口使用 JSON
- **关键服务**：ChatService（消息转发、踢人通知）、StatusService（负载均衡、Token 生成）、VarifyService（验证码发送）
- **构建流程**：CMake 通过 GrpcCodegen.cmake 自动从 .proto 文件生成 C++ 代码
- **注意**：VerifyServer 是 Node.js 实现，其他服务为 C++ 实现