---
kind: external_dependency
name: Node.js 邮件验证码服务
slug: nodejs
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
---

### Node.js 邮件验证码服务
- **角色**：独立的邮箱验证码派发服务，处理短信/邮件验证码的发送逻辑
- **集成点**：server/VarifyServer 目录下的 Node.js 应用，通过 gRPC 被 GateServer 调用
- **主要功能**：
  - 生成随机验证码
  - 发送邮件到指定邮箱
  - 验证码有效期管理（存储在 Redis）
- **技术栈**：Node.js + Express + nodemailer + redis
- **配置文件**：package.json 管理依赖，config.json 配置邮件服务参数
- **注意**：这是项目中唯一使用 JavaScript/Node.js 的服务，其他服务均为 C++ 实现