---
kind: external_dependency
name: Redis 缓存与分布式协调
slug: redis
category: external_dependency
category_hints:
    - vendor_identity
    - auth_protocol
scope:
    - '**'
---

### Redis 缓存与分布式协调
- **角色**：会话状态管理、Token 存储、分布式锁、负载均衡数据、断点续传进度
- **集成点**：通过 hiredis C 客户端库连接，封装在 RedisMgr 单例中
- **关键用途**：
  - Token 验证：`user_token_{uid}` 存储用户登录凭证
  - 会话管理：`userip_{uid}` 记录用户登录的 ChatServer 节点
  - 分布式锁：`lock_{uid}` 使用 `SET key uuid NX EX timeout` 实现
  - 负载均衡：`login_count` Hash 存储各 ChatServer 的连接数
  - 断点续传：存储文件传输进度（seq、已传大小）
- **认证方式**：默认端口 6380，支持密码认证（AUTH 命令）
- **数据结构**：广泛使用 String、Hash、List 等数据类型
- **注意**：项目使用同步 API，非异步 Redis 客户端