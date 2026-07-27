---
kind: external_dependency
name: Node.js 邮箱验证服务
slug: nodejs
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
source_files:
    - server/VarifyServer/server.js
    - server/VarifyServer/email.js
    - server/VarifyServer/redis.js
---

VarifyServer 使用 Node.js 实现邮箱验证码派发功能，依赖 @grpc/grpc-js 提供 gRPC 服务，ioredis 连接 Redis 存储验证码，nodemailer 发送邮件，uuid 生成唯一标识。该服务独立于 C++ 服务，专门处理注册、重置密码时的邮箱验证流程。