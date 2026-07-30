# 数据库存储过程(reg_user, reg_user_procedure)

<cite>
**本文引用的文件**   
- [llfc3.sql](file://sql备份/llfc3.sql)
- [day11-注册功能.md](file://开发文档/day11-注册功能.md)
- [MysqlDao.cpp](file://server/ChatServer/src/MysqlDao.cpp)
- [MysqlDao.h](file://server/ChatServer/include/MysqlDao.h)
</cite>

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构总览](#架构总览)
5. [详细组件分析](#详细组件分析)
6. [依赖关系分析](#依赖关系分析)
7. [性能考虑](#性能考虑)
8. [故障排查指南](#故障排查指南)
9. [结论](#结论)
10. [附录](#附录)

## 简介
本文围绕LLFCChat项目的两个用户注册存储过程 reg_user 与 reg_user_procedure，系统性说明其实现逻辑、事务处理机制、唯一性校验策略、用户ID生成方式、错误处理与回滚机制，以及OUT参数result的语义与使用方式。同时给出调用示例与异常处理最佳实践，帮助开发者正确集成与维护注册流程。

## 项目结构
- SQL脚本中定义了用户表user与自增序列表user_id，并包含两个注册存储过程的定义。
- C++服务层通过MysqlDao封装MySQL连接池与PreparedStatement调用，执行存储过程并读取OUT参数结果。
- 开发文档记录了存储过程创建思路与DAO层的调用方式。

```mermaid
graph TB
subgraph "数据库"
U["表 user"]
UID["表 user_id"]
P1["存储过程 reg_user"]
P2["存储过程 reg_user_procedure"]
end
subgraph "C++服务层"
DAO["MysqlDao::RegUser(...)"]
POOL["MySqlPool(连接池)"]
end
DAO --> POOL
DAO --> |"CALL reg_user(...)"| P1
DAO --> |"SELECT @result"| P1
P1 --> U
P1 --> UID
```

图表来源 
- [llfc3.sql:218-333](file://sql备份/llfc3.sql#L218-L333)
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)

章节来源
- [llfc3.sql:218-425](file://sql备份/llfc3.sql#L218-L425)
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)
- [day11-注册功能.md:555-601](file://开发文档/day11-注册功能.md#L555-L601)

## 核心组件
- 存储过程 reg_user：负责注册新用户，包含事务、唯一性检查、ID生成与插入，并通过OUT参数返回结果。
- 存储过程 reg_user_procedure：与reg_user逻辑一致，是同一功能的另一种命名版本。
- MysqlDao::RegUser：C++侧封装存储过程调用，设置输入参数，执行后通过会话变量读取OUT参数结果。

章节来源
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)

## 架构总览
注册流程从C++服务层发起，经连接池获取连接，准备并执行存储过程；存储过程内部进行事务控制、唯一性校验、ID分配与数据写入，最终通过OUT参数将结果返回给调用方。

```mermaid
sequenceDiagram
participant Client as "客户端/上层逻辑"
participant DAO as "MysqlDao : : RegUser"
participant Pool as "MySqlPool"
participant DB as "MySQL服务器"
participant Proc as "存储过程 reg_user"
participant TblU as "表 user"
participant TblUID as "表 user_id"
Client->>DAO : 调用 RegUser(name,email,pwd)
DAO->>Pool : 获取连接
DAO->>DB : PREPARE CALL reg_user(?,?,?,@result)
DAO->>DB : SET 输入参数 new_name,new_email,new_pwd
DAO->>DB : EXECUTE
DB->>Proc : 进入存储过程
Proc->>Proc : START TRANSACTION
Proc->>TblU : 检查 name/email 唯一性
alt 已存在
Proc-->>DAO : result=0
else 不存在
Proc->>TblUID : UPDATE id = id + 1
Proc->>TblUID : SELECT id INTO @new_id
Proc->>TblU : INSERT (uid,name,email,pwd)
Proc-->>DAO : result=@new_id
end
DAO->>DB : SELECT @result AS result
DB-->>DAO : 返回result值
DAO-->>Client : 返回int结果
```

图表来源 
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)

## 详细组件分析

### 存储过程 reg_user
- 入参：new_name、new_email、new_pwd（均为IN）
- 出参：result（INT，OUT）
- 事务：START TRANSACTION，成功COMMIT，异常ROLLBACK
- 唯一性检查：先查name，再查email
- ID生成：通过user_id表的自增列更新并取最新id作为uid
- 插入：向user表插入新记录，字段包括uid、name、email、pwd
- 返回值：
  - 0：用户名或邮箱已存在
  - >0：注册成功，返回新用户的uid
  - -1：发生异常（由异常处理器捕获并回滚）

```mermaid
flowchart TD
Start(["开始"]) --> TxStart["START TRANSACTION"]
TxStart --> CheckName{"是否存在同名用户?"}
CheckName --> |是| SetZero["SET result=0"] --> Commit["COMMIT"] --> End(["结束"])
CheckName --> |否| CheckEmail{"是否存在同邮箱用户?"}
CheckEmail --> |是| SetZero2["SET result=0"] --> Commit2["COMMIT"] --> End
CheckEmail --> |否| IncSeq["UPDATE user_id SET id=id+1"]
IncSeq --> GetId["SELECT id INTO @new_id FROM user_id"]
GetId --> InsertUser["INSERT INTO user(uid,name,email,pwd) VALUES(@new_id,...)"]
InsertUser --> SetResult["SET result=@new_id"] --> Commit3["COMMIT"] --> End
```

图表来源 
- [llfc3.sql:334-376](file://sql备份/llfc3.sql#L334-L376)

章节来源
- [llfc3.sql:334-376](file://sql备份/llfc3.sql#L334-L376)

### 存储过程 reg_user_procedure
- 与reg_user完全一致的逻辑与语义，仅名称不同，便于兼容或迁移。
- 同样采用事务、唯一性检查、user_id自增生成uid、插入user表、OUT参数result返回结果。

章节来源
- [llfc3.sql:379-425](file://sql备份/llfc3.sql#L379-L425)

### C++侧调用：MysqlDao::RegUser
- 从连接池获取连接
- 使用PreparedStatement调用存储过程：CALL reg_user(?,?,?,@result)
- 设置三个输入参数
- 执行后通过SELECT @result读取OUT参数
- 异常捕获：SQLException时打印错误信息并返回-1

```mermaid
classDiagram
class MysqlDao {
+RegUser(name, email, pwd) int
-pool_ : MySqlPool*
}
class MySqlPool {
+getConnection() Connection
+returnConnection(Connection) void
}
class MySQL {
+prepareStatement(sql) PreparedStatement
+executeQuery(sql) ResultSet
}
MysqlDao --> MySqlPool : "管理连接"
MysqlDao --> MySQL : "执行存储过程与查询"
```

图表来源 
- [MysqlDao.h:236-267](file://server/ChatServer/include/MysqlDao.h#L236-L267)
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)

章节来源
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)
- [MysqlDao.h:236-267](file://server/ChatServer/include/MysqlDao.h#L236-L267)

### 数据模型与约束
- 表 user：包含uid、name、email、pwd等字段；uid与email有唯一索引，name有普通索引
- 表 user_id：单行自增序列，用于生成连续uid

```mermaid
erDiagram
USER {
int id PK
int uid UK
varchar name
varchar email UK
varchar pwd
varchar nick
varchar desc
int sex
varchar icon
}
USER_ID {
int id PK
}
```

图表来源 
- [llfc3.sql:218-333](file://sql备份/llfc3.sql#L218-L333)

章节来源
- [llfc3.sql:218-333](file://sql备份/llfc3.sql#L218-L333)

## 依赖关系分析
- 存储过程依赖user与user_id两张表
- C++层依赖MySqlPool与MySQL驱动
- 存储过程之间无相互调用，但逻辑相同，可视为同一功能的不同入口

```mermaid
graph LR
DAO["MysqlDao::RegUser"] --> SP["存储过程 reg_user / reg_user_procedure"]
SP --> TBL_USER["表 user"]
SP --> TBL_UID["表 user_id"]
```

图表来源 
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)

章节来源
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)

## 性能考虑
- 唯一性检查两次查询（name与email），建议结合业务场景评估是否合并为一次查询或使用唯一索引保证一致性
- user_id自增更新为行级锁操作，在高并发注册场景可能成为热点瓶颈；可考虑分片或分布式ID方案
- 事务范围尽量短小，当前事务内仅包含必要操作，有利于减少锁持有时间
- 连接池复用连接，避免频繁建立/销毁连接开销

[本节为通用性能建议，不直接分析具体文件]

## 故障排查指南
- 常见返回码含义
  - 0：用户名或邮箱已存在
  - >0：注册成功，值为新uid
  - -1：异常或失败（存储过程异常处理器或C++层异常捕获）
- 常见问题定位
  - 连接池获取失败：检查连接池配置与数据库连通性
  - 存储过程执行异常：查看MySQL错误日志与SQLState
  - 唯一性冲突：确认user表中name与email的唯一索引是否生效
  - 序列表user_id为空：确保至少有一条记录以支持自增
- 调试建议
  - 在C++层打印SQLException详细信息（错误码、SQLState）
  - 在存储过程中增加关键步骤日志（如事务开始、检查条件、插入结果）

章节来源
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)

## 结论
reg_user与reg_user_procedure实现了统一的注册逻辑，具备事务保护、唯一性校验、ID生成与异常回滚能力。C++层通过MysqlDao封装存储过程调用，利用会话变量读取OUT参数result，形成清晰的调用链路。建议在高压场景下优化ID生成策略与唯一性检查路径，以提升吞吐与稳定性。

[本节为总结性内容，不直接分析具体文件]

## 附录

### OUT参数result的使用方式与返回值含义
- 存储过程通过OUT参数result返回结果：
  - 0：用户名或邮箱已存在
  - 正整数：注册成功，返回新用户的uid
  - -1：发生异常（事务回滚）
- C++侧通过SELECT @result读取结果，并在异常时返回-1

章节来源
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)

### 调用示例（基于现有代码）
- 准备语句：CALL reg_user(?,?,?,@result)
- 设置输入参数：new_name、new_email、new_pwd
- 执行后读取：SELECT @result AS result
- 根据返回码处理业务逻辑（0表示重复，>0表示成功，-1表示异常）

章节来源
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)
- [day11-注册功能.md:555-601](file://开发文档/day11-注册功能.md#L555-L601)

### 异常处理最佳实践
- 存储过程层面：使用DECLARE EXIT HANDLER捕获SQLEXCEPTION，执行ROLLBACK并将result置为-1
- C++层面：捕获SQLException，输出错误详情并返回-1，确保连接归还到连接池
- 业务层：对返回码进行分支处理，区分“重复”“成功”“异常”三类情况

章节来源
- [llfc3.sql:334-425](file://sql备份/llfc3.sql#L334-L425)
- [MysqlDao.cpp:19-57](file://server/ChatServer/src/MysqlDao.cpp#L19-L57)