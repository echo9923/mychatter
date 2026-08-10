# Redis缓存策略

<cite>
**本文引用的文件**
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)
- [StatusServiceImpl.cpp](file://server/StatusServer/src/StatusServiceImpl.cpp)
- [LogicSystem.cpp](file://server/ChatServer/src/LogicSystem.cpp)
- [LogicWorker.cpp](file://server/ResourceServer/src/LogicWorker.cpp)
- [const.h](file://server/ChatServer/include/const.h)
- [im_scenarios.cpp](file://tests/integration/im_scenarios.cpp)
- [DistLock.h](file://server/common/include/DistLock.h)
- [DistLock.cpp](file://server/common/src/DistLock.cpp)
- [RedisMgr.cpp](file://server/StatusServer/src/RedisMgr.cpp)
- [RedisMgr.cpp](file://server/GateServer/src/RedisMgr.cpp)
- [RedisMgr.cpp](file://server\ResourceServer\src\RedisMgr.cpp)
- [im_redis.cpp](file://tests/integration/im_redis.cpp)
</cite>

## 更新摘要
**变更内容**   
- **Redis命令升级**：所有服务的Redis管理器已从废弃的SETEX命令迁移到新的SET ... EX语法，确保与新版Redis兼容性
- **统一实现标准**：ChatServer、StatusServer、ResourceServer三个服务的RedisManager类都进行了相应的更新
- **向后兼容保证**：新语法在功能上完全等价于旧语法，但提供了更好的性能和未来兼容性
- **测试验证**：集成测试中的Redis封装也采用了新的SET ... EX语法

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构总览](#架构总览)
5. [详细组件分析](#详细组件分析)
6. [依赖关系分析](#依赖关系分析)
7. [性能与内存管理](#性能与内存管理)
8. [故障处理与高可用](#故障处理与高可用)
9. [键命名规范与过期策略](#键命名规范与过期策略)
10. [数据一致性保证](#数据一致性保证)
11. [缓存预热、失效与故障转移](#缓存预热失效与故障转移)
12. [高并发问题治理：穿透、雪崩、击穿](#高并发问题治理穿透雪崩击穿)
13. [监控与调优建议](#监控与调优建议)
14. [结论](#结论)

## 简介
本技术文档围绕 LLFCChat 的 Redis 缓存系统，系统性阐述连接池管理、序列化格式、缓存策略设计，以及用户会话缓存、好友关系缓存、在线状态缓存的实现方案。同时给出键命名规范、过期策略、一致性保障机制，覆盖缓存预热、失效处理与故障转移细节，并提供配置调优、内存管理与性能监控的最佳实践，以及在高并发场景下对缓存穿透、雪崩、击穿的解决方案。

**重大更新** 系统已简化Token验证机制，移除了复杂的chat_ticket票据管理，采用统一的utoken_<uid>键存储方案，TTL统一为86400秒（24小时）。所有服务器组件（GateServer、ChatServer、ResourceServer）都使用相同的token验证机制，消除了之前的复杂票据消费流程，显著提升了系统的可维护性和性能。**最新变更** 所有Redis管理器已全面升级到新的SET ... EX语法，替代了已废弃的SETEX命令，确保与新版Redis的完全兼容性。

## 项目结构
LLFCChat 在多个服务中复用统一的 Redis 抽象层：
- ChatServer、GateServer、ResourceServer、StatusServer 均包含 RedisMgr 与 RedisConPool 实现，用于连接池与命令封装。
- 常量定义集中存放于 const.h，统一了键前缀与分布式锁参数。
- 配置文件通过 ini 提供 Redis 主机、端口、密码等参数。

```mermaid
graph TB
subgraph "C++服务"
CS["ChatServer"]
GS["GateServer"]
RS["ResourceServer"]
SS["StatusServer"]
end
R["Redis 集群/单机"]
CS --> R
GS --> R
RS --> R
SS --> R
```

**图表来源**
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

**章节来源**
- [const.h](file://server/ChatServer/include/const.h)

## 核心组件
- **RedisConPool**：基于 hiredis 的连接池，负责连接创建、认证、获取/归还、空闲检测、异常重连与资源清理。
- **RedisMgr**：对外暴露 Get/Set/SetEx/LPush/RPush/HSet/HGet/HDel/Del/ExistsKey 等原子操作，以及分布式锁 acquire/release、登录计数统计和有序集合操作。
- **DistLock**：基于 SET NX EX + Lua 脚本的分布式锁实现，确保跨进程/跨服务的互斥访问。
- **常量与配置**：统一键前缀（如 USER_SESSION_PREFIX、USERIPPREFIX、USERTOKENPREFIX、LOGIN_COUNT、LOCK_PREFIX、OFFLINE_MSG_PREFIX 等）与锁超时参数。

**更新** Token验证机制已简化，USERTOKENPREFIX ("utoken_") 成为统一的token存储前缀，配合SetEx方法实现原子性的token设置和过期管理。**最新升级** SetEx方法现已使用新的SET ... EX语法，替代了已废弃的SETEX命令。

**章节来源**
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)
- [const.h](file://server/ChatServer/include/const.h)

## 架构总览
整体采用"服务进程内单例 RedisMgr + 连接池"模式，所有 Redis 访问通过统一接口完成；分布式锁由 DistLock 提供；业务侧（如心跳、踢人、登录计数、离线消息处理）通过 RedisMgr 调用。

```mermaid
classDiagram
class RedisConPool {
+getConnection()
+getConNonBlock()
+returnConnection(ctx)
+Close()
-reconnect()
-checkThreadPro()
}
class RedisMgr {
+Get(key, value) bool
+Set(key, value) bool
+SetEx(key, ttl_seconds, value) bool
+LPush/RPush/LPop/RPop(...)
+HSet/HGet/HDel(...)
+Del/ExistsKey(...)
+ZAdd/ZRangeByScore/ZRem/Expire(...)
+acquireLock(lockName, lockTimeout, acquireTimeout) string
+releaseLock(lockName, identifier) bool
+IncreaseCount/DecreaseCount/InitCount/DelCount(server_name)
}
class DistLock {
+acquireLock(context, lockName, lockTimeout, acquireTimeout) string
+releaseLock(context, lockName, identifier) bool
}
RedisMgr --> RedisConPool : "使用连接池"
RedisMgr --> DistLock : "分布式锁"
```

**图表来源**
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)
- [DistLock.h](file://server/common/include/DistLock.h)

**章节来源**
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

## 详细组件分析

### 连接池与生命周期管理
- **初始化**：构造时按配置创建固定数量连接并执行 AUTH。
- **获取/归还**：线程安全队列，条件变量唤醒等待者；非阻塞获取用于健康检查。
- **健康检查**：后台线程周期性 PING，失败连接释放并计数，随后尝试重建连接。
- **关闭**：设置停止标志，通知所有等待线程，回收全部连接。

```mermaid
sequenceDiagram
participant T as "业务线程"
participant Pool as "RedisConPool"
participant R as "Redis"
T->>Pool : getConnection()
alt 有可用连接
Pool-->>T : redisContext*
T->>R : 执行命令
R-->>T : 返回结果
T->>Pool : returnConnection(ctx)
else 无可用连接
Pool-->>T : 等待(条件变量)
T->>Pool : getConnection() (被唤醒)
Pool-->>T : redisContext*
end
```

**章节来源**
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

### 分布式锁实现
- **加锁**：SET key identifier NX EX timeout，带唯一标识与过期时间，避免死锁。
- **解锁**：EVAL Lua 脚本比较标识后删除，保证仅持有者可释放。
- **重试**：在 acquireTimeout 内轮询，间隔短睡眠降低竞争。

```mermaid
flowchart TD
Start(["开始"]) --> GenID["生成唯一标识"]
GenID --> TryLock["SET lockKey identifier NX EX timeout"]
TryLock --> |成功| ReturnID["返回identifier"]
TryLock --> |失败| CheckTime{"是否超过acquireTimeout?"}
CheckTime --> |否| Sleep["短暂休眠"]
Sleep --> TryLock
CheckTime --> |是| Fail["返回空字符串"]
ReturnID --> End(["结束"])
Fail --> End
```

**图表来源**
- [DistLock.cpp](file://server/common/src/DistLock.cpp)

**章节来源**
- [DistLock.cpp](file://server/common/src/DistLock.cpp)

### 简化的Token验证机制

**重大更新** 系统已完全移除了复杂的chat_ticket票据管理机制，采用统一的utoken_<uid>键存储方案：

#### Token生成与存储（StatusServer）
- **生成唯一token**：使用UUID生成器创建随机token字符串
- **原子性存储**：通过SetEx命令将token存储在`utoken_<uid>`键中，TTL设置为86400秒（24小时）
- **错误处理**：Redis失败时返回RPCFailed错误码

#### Token验证（ChatServer & ResourceServer）
- **统一验证逻辑**：所有服务都从Redis中读取`utoken_<uid>`键进行验证
- **严格匹配**：必须存在且值完全匹配请求中的token
- **失败处理**：缺失或不匹配一律返回TokenInvalid错误，不建立会话

```mermaid
sequenceDiagram
participant Client as "客户端"
participant Gate as "GateServer"
participant Status as "StatusServer"
participant Redis as "Redis"
participant Chat as "ChatServer"
participant Resource as "ResourceServer"
Client->>Gate : /user_login(uid, password)
Gate->>Status : GetChatServer(uid)
Status->>Redis : SetEx("utoken_"+uid, 86400, token)
Status-->>Gate : server_info + token
Gate-->>Client : chat_server_addr + token
Client->>Chat : Login(uid, token)
Chat->>Redis : Get("utoken_"+uid)
Redis-->>Chat : stored_token
alt token匹配
Chat-->>Client : Success + user_info
else token不匹配
Chat-->>Client : TokenInvalid
end
Client->>Resource : Login(uid, token)
Resource->>Redis : Get("utoken_"+uid)
Redis-->>Resource : stored_token
alt token匹配
Resource-->>Client : Success + uid
else token不匹配
Resource-->>Client : TokenInvalid
end
```

**图表来源**
- [StatusServiceImpl.cpp](file://server/StatusServer/src/StatusServiceImpl.cpp)
- [LogicSystem.cpp](file://server/ChatServer/src/LogicSystem.cpp)
- [LogicWorker.cpp](file://server/ResourceServer/src/LogicWorker.cpp)

**章节来源**
- [StatusServiceImpl.cpp](file://server/StatusServer/src/StatusServiceImpl.cpp)
- [LogicSystem.cpp](file://server/ChatServer/src/LogicSystem.cpp)
- [LogicWorker.cpp](file://server/ResourceServer/src/LogicWorker.cpp)

### SetEx方法与原子性操作

**重要更新** 所有服务的Redis管理器已全面升级到新的SET ... EX语法：

#### 新语法实现
- **ChatServer**: `redisCommand(connect, "SET %s %s EX %d", key.c_str(), value.c_str(), ttl_seconds)`
- **StatusServer**: `redisCommand(connect, "SET %s %s EX %d", key.c_str(), value.c_str(), ttl_seconds)`
- **ResourceServer**: `redisCommand(connect, "SET %s %s EX %d", key.c_str(), value.c_str(), expire_seconds)`
- **GateServer**: 使用标准的Set方法，不带过期时间

#### 语法对比
- **旧语法（已废弃）**：`SETEX key seconds value`
- **新语法（推荐）**：`SET key value EX seconds`

#### 优势特性
- **原子性保证**：SET ... EX命令同时设置值和过期时间，避免竞态条件
- **统一TTL**：所有token的TTL统一为86400秒（24小时），简化过期管理
- **自动清理**：过期时间到期后自动删除，无需手动清理
- **兼容性**：与新版Redis完全兼容，支持未来的语法扩展

```mermaid
flowchart TD
A["用户登录请求"] --> B["StatusServer生成UUID token"]
B --> C["SET 'utoken_'+uid token EX 86400"]
C --> D["Redis原子性存储"]
D --> E["返回token给客户端"]
E --> F["客户端携带token访问Chat/Resource"]
F --> G["Get('utoken_'+uid)验证token"]
G --> H{"token匹配?"}
H --> |是| I["建立会话"]
H --> |否| J["返回TokenInvalid"]
```

**图表来源**
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)
- [RedisMgr.cpp](file://server/StatusServer/src/RedisMgr.cpp)
- [RedisMgr.cpp](file://server\ResourceServer\src\RedisMgr.cpp)

**章节来源**
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)
- [RedisMgr.cpp](file://server/StatusServer/src/RedisMgr.cpp)
- [RedisMgr.cpp](file://server\ResourceServer\src\RedisMgr.cpp)

### 有序集合操作与离线消息处理
- **ZAdd()**：向有序集合添加成员及其分值，使用二进制安全的argv方式传递参数，避免特殊字符问题。
- **ZRangeByScore()**：按分值范围查询有序集合成员，支持排他下界和LIMIT分页。
- **ZRem()**：从有序集合移除指定成员，用于消息确认后的清理。
- **Expire()**：为键设置过期时间，配合有序集合实现自动清理。

**章节来源**
- [LogicSystem.cpp](file://server/ChatServer/src/LogicSystem.cpp)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

### 登录计数与服务器负载统计
- **使用 HASH 存储**各服务名对应的当前连接数，定时任务汇总写入。
- **增删计数**通过分布式锁保护，避免并发竞态。

**章节来源**
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

## 依赖关系分析
- **C++ 服务**依赖 hiredis 库进行网络 I/O 与协议解析。
- **RedisMgr**依赖 ConfigMgr 读取 Redis 配置（Host/Port/Passwd）。
- **DistLock**依赖 Boost UUID 生成唯一标识。

```mermaid
graph LR
RedisMgr["RedisMgr.cpp"] --> Hiredis["hiredis"]
RedisMgr --> ConfigMgr["ConfigMgr"]
DistLock["DistLock.cpp"] --> BoostUUID["Boost UUID"]
```

**图表来源**
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)
- [DistLock.cpp](file://server/common/src/DistLock.cpp)

**章节来源**
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)
- [DistLock.cpp](file://server/common/src/DistLock.cpp)

## 性能与内存管理
- **连接池大小**：ChatServer 默认 10，可按 QPS 与延迟目标调整。
- **命令开销**：SetEx命令具有原子性，避免了额外的EXPIRE命令调用。
- **内存释放**：每次命令后显式 freeReplyObject，避免内存泄漏。
- **心跳与健康检查**：后台线程每 60 秒触发一次全量 PING，失败连接及时重建。

**更新** 简化的token验证机制减少了Redis命令调用次数，提升了整体性能。**最新优化** 新的SET ... EX语法相比旧的SETEX命令提供了更好的性能和未来兼容性，同时保持了相同的功能特性。

**章节来源**
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)

## 故障处理与高可用
- **连接失败**：getConnection 返回 nullptr，上层快速失败；健康检查线程持续重建。
- **认证失败**：AUTH 失败跳过该连接，记录日志。
- **重连策略**：checkThreadPro 统计失败数，循环尝试 reconnect，直至恢复或达到上限。
- **优雅关闭**：Close 设置停止位，唤醒等待线程，join 检查线程，释放所有连接。

**更新** Token验证失败时，系统会立即返回TokenInvalid错误，不会建立任何会话，确保了安全性。**最新改进** 新的SET ... EX语法提供了更可靠的错误处理和更好的兼容性保证。

**章节来源**
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

## 键命名规范与过期策略
- **键前缀与用途**（来自 const.h）：
  - USER_SESSION_PREFIX：用户会话信息（usession_）
  - USERIPPREFIX：用户IP绑定（uip_）
  - USERTOKENPREFIX：**用户令牌（utoken_）** - 简化后的统一token存储
  - IPCOUNTPREFIX：IP计数（ipcount_）
  - USER_BASE_INFO：基础用户信息（ubaseinfo_）
  - NAME_INFO：名称索引（nameinfo_）
  - LOGIN_COUNT：各服务登录计数（logincount）
  - LOCK_PREFIX：分布式锁前缀（lock_）
  - OFFLINE_MSG_PREFIX：离线消息有序集合（offline_msg:）
  - CHATSERVER_HEARTBEAT_PREFIX：ChatServer心跳键（chatserver_heartbeat:）
  - CHATSERVER_INFO_KEY：ChatServer节点信息（chatserver_info）
- **过期策略**：
  - **Token统一TTL**：所有utoken_*键的TTL统一设置为86400秒（24小时）
  - 会话与令牌建议使用 EX/PX 设置 TTL，结合心跳刷新。
  - 登录计数为持久型 HASH，不设置过期。
  - 分布式锁通过 EX 自动过期，防止死锁。
  - 离线消息通过 Expire() 设置 TTL，实现自动清理。
  - 心跳键通过 SetWithExpire() 设置TTL，自动清理过期节点。

**重大更新** 键命名规范已简化，USERTOKENPREFIX ("utoken_") 成为唯一的token存储前缀，配合统一的86400秒TTL策略，大大简化了token管理逻辑。**最新升级** 所有TTL设置现在都使用新的SET ... EX语法，确保与新版Redis的完全兼容性。

**章节来源**
- [const.h](file://server/ChatServer/include/const.h)

## 数据一致性保证
- **分布式锁**：加锁-执行业务-解锁三段式，Lua 脚本保证判断与删除原子性。
- **会话一致性**：心跳周期内以 Redis 中的 session_id 为准，多端登录冲突时以最新为准。
- **计数一致性**：登录计数更新前加锁，避免并发叠加或丢失。
- **有序集合一致性**：ZAdd/ZRem操作保证消息顺序的一致性，Expire确保过期数据的自动清理。
- **Token一致性**：**SetEx命令确保token设置的原子性，Get验证确保token匹配的准确性**。

**重大更新** Token验证机制的数据一致性得到了显著提升，SetEx的原子性保证了token设置的可靠性，统一的验证逻辑确保了跨服务的一致性。**最新改进** 新的SET ... EX语法提供了更强的原子性保证和更好的错误处理能力。

**章节来源**
- [DistLock.cpp](file://server/common/src/DistLock.cpp)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

## 缓存预热、失效与故障转移
- **预热**：服务启动后可根据热点键（如热门用户信息、常用配置）批量加载至 Redis。
- **失效**：业务变更时主动 Del/HDel 对应键；会话类键通过 TTL 自然失效。
- **故障转移**：连接池健康检查与自动重连；若 Redis 不可用，上层应降级（如本地缓存或限流）。
- **服务发现降级**：当Redis不可用时，StatusServer回退到本地静态配置提供服务。

**更新** Token验证失败时，系统会立即拒绝请求，不会建立任何会话，确保了系统的安全性。**最新增强** 新的SET ... EX语法在故障情况下提供了更好的错误报告和恢复能力。

**章节来源**
- [RedisMgr.h](file://server/ChatServer/include/RedisMgr.h)
- [RedisMgr.cpp](file://server/ChatServer/src/RedisMgr.cpp)

## 高并发问题治理：穿透、雪崩、击穿
- **穿透**（查询不存在的数据）：
  - 布隆过滤器拦截非法键；或为不存在键设置短 TTL 的空值占位。
- **雪崩**（大量键同时过期）：
  - 过期时间加入随机抖动；热点键采用逻辑过期+后台异步重建。
  - **Token雪崩防护**：86400秒的较长TTL减少了频繁过期的风险。
- **击穿**（热点键瞬时失效）：
  - 加分布式锁保证只有一线程回源重建；或使用互斥信号量控制重建并发。
- **有序集合雪崩**：
  - 为有序集合设置合理的TTL，避免大量有序集合同时过期。
  - **服务发现雪崩**：HGetAll操作本身具有原子性，但需考虑Redis整体可用性；可引入本地缓存层。

**更新** 简化的token验证机制减少了高并发场景下的复杂性，统一的TTL策略降低了雪崩风险。**最新优化** 新的SET ... EX语法在高并发环境下提供了更好的性能和稳定性。

## 监控与调优建议
- **关键指标**：
  - 连接池大小、活跃连接数、命中率、平均/尾延迟、错误率、重连次数。
  - 有序集合操作成功率、过期键清理效率。
  - 心跳键存活率、服务发现响应时间。
  - **Token验证成功率、Redis SetEx/Get操作延迟**。
- **配置调优**：
  - 连接池大小按峰值 QPS×平均 RT/1s 估算；适当增大以减少等待。
  - 合理设置心跳与健检频率，平衡 CPU 与可靠性。
  - 离线消息TTL根据业务需求调整，平衡内存占用和数据保留时间。
  - 心跳TTL设置为10-30秒，平衡实时性和网络开销。
  - **Token TTL保持86400秒，平衡安全性和用户体验**。
- **监控手段**：
  - 采集 Redis 服务端 stats（connected_clients、keyspace_hits/misses、used_memory）。
  - 应用侧埋点：命令耗时、失败次数、锁等待时长。
  - 监控有序集合大小变化趋势，及时发现内存增长问题。
  - 监控服务发现成功率，及时发现节点异常。
  - **监控Token验证失败率，及时发现安全问题**。

**更新** 新增了对Token验证相关指标的监控建议，包括验证成功率、Redis操作延迟等关键指标。**最新建议** 建议监控SET ... EX命令的执行情况，确保新语法的正确部署和性能表现。

## 结论
LLFCChat 的 Redis 子系统以连接池为核心，配合分布式锁与统一命令封装，实现了稳定可靠的缓存与状态管理能力。**重大更新** 随着Token验证机制的简化，系统已移除了复杂的chat_ticket票据管理，采用统一的utoken_<uid>键存储方案，TTL统一为86400秒（24小时）。这一改进显著提升了系统的可维护性和性能，所有服务器组件（GateServer、ChatServer、ResourceServer）都使用相同的token验证机制，消除了之前的复杂票据消费流程。**最新升级** 所有Redis管理器已全面升级到新的SET ... EX语法，替代了已废弃的SETEX命令，确保与新版Redis的完全兼容性。

通过规范的键命名、合理的过期策略与健壮的错误处理，支撑了会话、在线状态、计数、有序集合和服务发现等关键场景。面向高并发与高可用，建议进一步引入批量/管道、热点键保护、多级缓存与完善的监控告警体系，持续提升系统吞吐与稳定性。

**简化的Token验证机制与新的SET ... EX语法为LLFCChat提供了更加可靠和高效的认证方案，通过原子性的操作和统一的验证逻辑，实现了跨服务一致的token管理，形成了简洁而强大的分布式认证体系。**