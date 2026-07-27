---
kind: external_dependency
name: MySQL 数据库
slug: mysql
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
source_files:
    - server/ChatServer/include/MysqlMgr.h
    - sql备份/llfc.sql
---

使用 MySQL Connector/C++ 作为数据库访问层，通过 MysqlMgr 单例类统一管理数据库操作。主要存储用户信息（user表）、聊天消息（chat_message表）、好友关系（friend表）、好友申请（friend_apply表）、聊天会话（chat_thread表）等。数据库设计包含索引优化（如 idx_thread_created、idx_thread_message），支持分页查询和事务操作。