---
kind: external_dependency
name: MySQL 数据库
slug: mysql
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
---

### MySQL 数据库
- **角色**：持久化存储用户信息、聊天消息、好友关系等结构化数据
- **集成点**：通过 mysql-connector-cpp JDBC 驱动连接，封装在 MysqlMgr 单例中
- **主要用途**：
  - 用户注册/登录验证（用户名、邮箱、密码）
  - 聊天消息持久化存储
  - 好友申请和好友关系管理
  - 用户基本信息查询（头像、昵称等）
- **连接管理**：使用连接池管理数据库连接，避免频繁建立连接开销
- **事务支持**：部分操作（如添加好友）使用数据库事务保证一致性
- **注意**：项目使用同步 SQL 操作，可能成为性能瓶颈