---
kind: external_dependency
name: Redis 缓存与分布式锁
slug: redis
category: external_dependency
category_hints:
    - vendor_identity
scope:
    - '**'
source_files:
    - server/ChatServer/include/RedisMgr.h
    - server/ChatServer/src/DistLock.cpp
---

Redis 在项目中有多个关键用途：存储用户 Token（utoken_{uid}）、记录用户登录状态（userip_{uid}）、实现分布式锁（lock_{uid}）、缓存用户基本信息（ubaseinfo_{uid}）、统计登录次数（logincount）。使用 hiredis C 客户端库进行连接管理。分布式锁通过 SET lockKey identifier NX EX lockTimeout 命令实现，支持超时释放和死锁检测。