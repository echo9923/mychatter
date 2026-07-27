---
kind: external_dependency
name: gRPC 微服务通信框架
slug: grpc
category: external_dependency
category_hints:
    - framework_behavior
scope:
    - '**'
source_files:
    - server/*/include/*GrpcClient.h
    - server/proto/
    - server/VarifyServer/message.proto
---

项目使用 gRPC 作为微服务间通信协议，基于 protobuf 定义接口。主要应用场景包括：GateServer 调用 StatusServer 获取 ChatServer 节点信息、ChatServer 间的跨服通知（踢人、好友申请、消息转发）、VarifyServer 的验证码服务。C++ 服务端和 Node.js 的 VarifyServer 通过 @grpc/grpc-js 进行通信。gRPC 采用同步调用模式，在业务线程中直接阻塞等待响应。