# ChatServer聊天核心服务

<cite>
**本文引用的文件**   
- [CSession.h](file://server/ChatServer/include/CSession.h)
- [CSession.cpp](file://server/ChatServer/src/CSession.cpp)
- [LogicSystem.h](file://server/ChatServer/include/LogicSystem.h)
- [LogicSystem.cpp](file://server/ChatServer/src/LogicSystem.cpp)
- [UserMgr.h](file://server/ChatServer/include/UserMgr.h)
- [UserMgr.cpp](file://server/ChatServer/src/UserMgr.cpp)
- [DistLock.h](file://server/ChatServer/include/DistLock.h)
- [DistLock.cpp](file://server/ChatServer/src/DistLock.cpp)
- [data.h](file://server/ChatServer/include/data.h)
- [const.h](file://server/ChatServer/include/const.h)
- [MsgNode.h](file://server/ChatServer/include/MsgNode.h)
- [chat.proto](file://proto/chat_service/chat.proto)
- [CServer.cpp](file://server/ChatServer/src/CServer.cpp)
- [chatserver1.ini](file://server/ChatServer/config/chatserver1.ini)
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
10. [附录：消息协议与错误码](#附录消息协议与错误码)

## 简介
本开发文档面向ChatServer聊天核心服务，聚焦其作为实时聊天消息处理中心的职责：TCP连接管理、消息路由与会话维护。重点阐述以下方面：
- CSession类连接生命周期管理（建立、心跳检测、收发、断开）
- LogicSystem业务逻辑处理（消息验证、权限检查、广播机制）
- UserMgr用户状态管理（在线用户列表、好友关系、消息队列）
- 分布式锁实现（多进程数据一致性）
- 消息协议定义、错误处理策略与性能优化方案

## 项目结构
ChatServer位于server/ChatServer目录，采用分层组织：
- include：头文件定义（会话、逻辑系统、用户管理、分布式锁、数据结构、常量等）
- src：实现文件（IO、业务逻辑、存储访问、配置管理等）
- config：运行期配置文件（端口、RPC、数据库、Redis、对端服务等）
- proto：gRPC接口定义（跨服务通信协议）

```mermaid
graph TB
subgraph "ChatServer"
A["CServer<br/>监听与Accept"] --> B["CSession<br/>TCP会话"]
B --> C["LogicSystem<br/>消息分发与处理"]
C --> D["UserMgr<br/>在线会话映射"]
C --> E["MysqlMgr/RedisMgr<br/>持久化与缓存"]
C --> F["ChatGrpcClient<br/>跨服务通知"]
G["DistLock<br/>Redis分布式锁"] --> C
H["ConfigMgr<br/>配置读取"] --> C
end
I["客户端"] --> A
```

图表来源 
- [CServer.cpp:1-133](file://server/ChatServer/src/CServer.cpp#L1-L133)
- [CSession.h:1-86](file://server/ChatServer/include/CSession.h#L1-L86)
- [LogicSystem.h:1-60](file://server/ChatServer/include/LogicSystem.h#L1-L60)
- [UserMgr.h:1-22](file://server/ChatServer/include/UserMgr.h#L1-L22)
- [DistLock.h:1-18](file://server/ChatServer/include/DistLock.h#L1-L18)
- [chat.proto:1-105](file://proto/chat_service/chat.proto#L1-L105)

章节来源
- [CServer.cpp:1-133](file://server/ChatServer/src/CServer.cpp#L1-L133)
- [chatserver1.ini:1-30](file://server/ChatServer/config/chatserver1.ini#L1-L30)

## 核心组件
- CSession：封装单个TCP连接的读写、粘包处理、发送队列、心跳计时、异常清理。
- LogicSystem：单例消息总线，按消息ID分派到具体处理器；包含登录、搜索、好友申请/认证、文本/图片聊天、心跳、线程加载等。
- UserMgr：维护uid到session的映射，提供获取、设置、移除操作。
- DistLock：基于Redis的分布式锁，支持超时与原子释放。
- 数据模型：UserInfo、ApplyInfo、ChatThreadInfo、ChatMessage、PageResult、ChatMsgType。
- 常量与协议：ErrorCodes、MSG_IDS、Redis键前缀、gRPC消息类型。

章节来源
- [CSession.h:1-86](file://server/ChatServer/include/CSession.h#L1-L86)
- [LogicSystem.h:1-60](file://server/ChatServer/include/LogicSystem.h#L1-L60)
- [UserMgr.h:1-22](file://server/ChatServer/include/UserMgr.h#L1-L22)
- [DistLock.h:1-18](file://server/ChatServer/include/DistLock.h#L1-L18)
- [data.h:1-65](file://server/ChatServer/include/data.h#L1-L65)
- [const.h:1-104](file://server/ChatServer/include/const.h#L1-L104)

## 架构总览
ChatServer通过AsioIOServicePool进行高并发IO，CServer负责Accept新连接并创建CSession；CSession解析头部与体，将消息投递至LogicSystem队列；LogicSystem在独立工作线程中消费并调用对应处理器；处理器根据目标用户所在服务器选择本地推送或gRPC跨服通知；UserMgr维护内存中的uid->session映射；分布式锁保证关键路径（如踢人、登录互斥）的一致性。

```mermaid
sequenceDiagram
participant Client as "客户端"
participant Server as "CServer"
participant Session as "CSession"
participant Logic as "LogicSystem"
participant UMgr as "UserMgr"
participant Redis as "Redis"
participant DB as "MySQL"
participant Peer as "其他ChatServer(gRPC)"
Client->>Server : TCP连接
Server-->>Session : 创建并Start()
Session->>Session : 读头部(HEAD_TOTAL_LEN)
Session->>Session : 读正文(长度由头部决定)
Session->>Logic : PostMsgToQue(LogicNode)
Logic->>Logic : 工作线程消费队列
Logic->>Logic : 按msg_id路由到处理器
alt 登录流程
Logic->>Redis : 校验Token
Logic->>DB : 拉取用户基础信息
Logic->>Redis : 分布式锁(lock_+uid)
Logic->>Peer : 跨服踢人(必要时)
Logic->>UMgr : SetUserSession(uid, session)
Logic-->>Client : 登录响应(含好友/申请列表)
else 文本聊天
Logic->>DB : 写入聊天记录
Logic->>Redis : 查询接收方所在服务器
alt 同服
Logic->>UMgr : GetSession(touid)
Logic-->>Client : 推送通知
else 跨服
Logic->>Peer : gRPC NotifyTextChatMsg
end
end
```

图表来源 
- [CServer.cpp:1-133](file://server/ChatServer/src/CServer.cpp#L1-L133)
- [CSession.cpp:1-335](file://server/ChatServer/src/CSession.cpp#L1-L335)
- [LogicSystem.cpp:1-945](file://server/ChatServer/src/LogicSystem.cpp#L1-L945)
- [UserMgr.cpp:1-50](file://server/ChatServer/src/UserMgr.cpp#L1-L50)
- [chat.proto:1-105](file://proto/chat_service/chat.proto#L1-L105)

## 详细组件分析

### CSession：连接生命周期与I/O
- 连接建立：构造函数生成唯一session_id，初始化接收缓冲与心跳时间戳；Start()启动头部读取。
- 粘包处理：AsyncReadHead读取固定长度的头部，解析msg_id与msg_len后，进入AsyncReadBody读取完整正文。
- 发送队列：Send将消息入队，使用互斥保护；若队列为空则立即发起异步写，HandleWrite完成后继续出队下一个。
- 心跳检测：每次成功读取更新_last_heartbeat；IsHeartbeatExpired判断是否超过阈值（默认20秒）。
- 异常处理：网络错误或长度不匹配时关闭socket并触发DealExceptionSession，清理Redis中的会话、IP、Token等信息。
- 离线/图片通知：NotifyOffline与NotifyChatImgRecv构造JSON并通过Send下发。

```mermaid
classDiagram
class CSession {
+GetSocket() tcp : : socket&
+GetSessionId() string&
+SetUserId(int)
+GetUserId() int
+Start()
+Send(string, short)
+Send(char*, short, short)
+Close()
+SharedSelf() shared_ptr<CSession>
+AsyncReadBody(int)
+AsyncReadHead(int)
+NotifyOffline(int)
+NotifyChatImgRecv(request*)
+IsHeartbeatExpired(now) bool
+UpdateHeartbeat()
+DealExceptionSession()
-asyncReadFull(size_t, handler)
-asyncReadLen(size_t, size_t, handler)
-HandleWrite(error_code, shared_ptr<CSession>)
-_socket : tcp : : socket
-_session_id : string
-_data : char[MAX_LENGTH]
-_send_que : queue<SendNode>
-_recv_msg_node : RecvNode
-_user_uid : int
-_last_heartbeat : time_t
-_session_mtx : mutex
}
class MsgNode {
+Clear()
+_cur_len : short
+_total_len : short
+_data : char*
}
class SendNode {
-_msg_id : short
}
class RecvNode {
-_msg_id : short
}
CSession --> MsgNode : "使用"
CSession --> SendNode : "发送队列"
CSession --> RecvNode : "接收缓冲"
```

图表来源 
- [CSession.h:1-86](file://server/ChatServer/include/CSession.h#L1-L86)
- [MsgNode.h:1-48](file://server/ChatServer/include/MsgNode.h#L1-L48)

章节来源
- [CSession.cpp:1-335](file://server/ChatServer/src/CSession.cpp#L1-L335)
- [CSession.h:1-86](file://server/ChatServer/include/CSession.h#L1-L86)
- [MsgNode.h:1-48](file://server/ChatServer/include/MsgNode.h#L1-L48)

### LogicSystem：业务逻辑与消息路由
- 消息队列与工作线程：PostMsgToQue将LogicNode入队，条件变量唤醒DealMsg；DealMsg循环取出并按msg_id查找回调函数执行。
- 回调注册：RegisterCallBacks绑定各业务处理器（登录、搜索、好友申请/认证、文本/图片聊天、心跳、线程加载等）。
- 登录流程：校验Token、拉取用户基础信息、分布式锁保护登录互斥、跨服踢人、绑定session与uid、返回好友与申请列表。
- 文本聊天：批量插入数据库，查询接收方所在服务器，同服直接推送，跨服通过gRPC通知。
- 图片聊天：与文本类似，但走图片通道并通知客户端下载。
- 错误处理：统一使用Defer在析构时回写响应；失败时填充ErrorCodes。

```mermaid
flowchart TD
Start(["进入处理器"]) --> Parse["解析JSON参数"]
Parse --> Validate{"参数合法?"}
Validate --> |否| Err["填充错误码并返回"]
Validate --> |是| Route{"按msg_id路由"}
Route --> Login["登录处理器"]
Route --> Text["文本聊天处理器"]
Route --> Img["图片聊天处理器"]
Route --> Heart["心跳处理器"]
Route --> Other["其他处理器"]
Login --> CheckToken["校验Token(Redis)"]
CheckToken --> Lock["分布式锁(lock_+uid)"]
Lock --> KickCheck{"是否异地登录?"}
KickCheck --> |是| Kick["跨服踢人或本地踢人"]
KickCheck --> |否| Bind["绑定uid-session"]
Bind --> Resp["返回登录结果"]
Text --> SaveDB["写入聊天记录"]
SaveDB --> FindTarget["查询接收方服务器"]
FindTarget --> SameSrv{"同服?"}
SameSrv --> |是| PushLocal["本地推送"]
SameSrv --> |否| PushRemote["gRPC跨服通知"]
PushLocal --> Resp
PushRemote --> Resp
Img --> SaveDB
Heart --> Resp
Other --> Resp
Resp --> End(["结束"])
```

图表来源 
- [LogicSystem.cpp:1-945](file://server/ChatServer/src/LogicSystem.cpp#L1-L945)
- [const.h:1-104](file://server/ChatServer/include/const.h#L1-L104)

章节来源
- [LogicSystem.cpp:1-945](file://server/ChatServer/src/LogicSystem.cpp#L1-L945)
- [LogicSystem.h:1-60](file://server/ChatServer/include/LogicSystem.h#L1-L60)
- [const.h:1-104](file://server/ChatServer/include/const.h#L1-L104)

### UserMgr：用户状态管理
- 在线会话映射：内部unordered_map<int, shared_ptr<CSession>>，加锁保护。
- 主要方法：GetSession、SetUserSession、RmvUserSession（校验session_id一致性，避免误删异地登录）。
- 与CServer协作：CServer在清除session时调用RmvUserSession，确保内存映射与真实连接一致。

```mermaid
classDiagram
class UserMgr {
+GetSession(uid) shared_ptr<CSession>
+SetUserSession(uid, session)
+RmvUserSession(uid, session_id)
-_session_mtx : mutex
-_uid_to_session : unordered_map<int, shared_ptr<CSession>>
}
class CSession {
+GetSessionId() string&
+GetUserId() int
}
UserMgr --> CSession : "持有引用"
```

图表来源 
- [UserMgr.h:1-22](file://server/ChatServer/include/UserMgr.h#L1-L22)
- [UserMgr.cpp:1-50](file://server/ChatServer/src/UserMgr.cpp#L1-L50)
- [CSession.h:1-86](file://server/ChatServer/include/CSession.h#L1-L86)

章节来源
- [UserMgr.cpp:1-50](file://server/ChatServer/src/UserMgr.cpp#L1-L50)
- [UserMgr.h:1-22](file://server/ChatServer/include/UserMgr.h#L1-L22)

### 分布式锁：DistLock
- 获取锁：使用Redis SET key identifier NX EX timeout，轮询直至成功或超时。
- 释放锁：EVAL Lua脚本比较identifier并原子删除，防止误删他人锁。
- 使用场景：登录互斥、踢人、关键资源更新等。

```mermaid
flowchart TD
A["请求获取锁"] --> B["生成唯一identifier"]
B --> C["SET lock:key identifier NX EX timeout"]
C --> D{"是否OK?"}
D --> |是| E["返回identifier"]
D --> |否| F["sleep 1ms重试"]
F --> C
E --> G["执行业务逻辑"]
G --> H["EVAL Lua: 比较identifier并del"]
H --> I{"是否成功?"}
I --> |是| J["释放完成"]
I --> |否| K["忽略(非本人锁)"]
```

图表来源 
- [DistLock.cpp:1-73](file://server/ChatServer/src/DistLock.cpp#L1-L73)
- [DistLock.h:1-18](file://server/ChatServer/include/DistLock.h#L1-L18)

章节来源
- [DistLock.cpp:1-73](file://server/ChatServer/src/DistLock.cpp#L1-L73)
- [DistLock.h:1-18](file://server/ChatServer/include/DistLock.h#L1-L18)

### CServer：定时器与心跳清理
- Accept循环：从AsioIOServicePool获取IO上下文，异步接受连接，创建CSession并Start。
- 定时任务：每60秒遍历会话副本，检测心跳过期，关闭socket并收集待清理会话；统计在线数写入Redis。
- 清理流程：对过期会话调用DealExceptionSession，最终清理Redis中的会话、IP、Token等。

章节来源
- [CServer.cpp:1-133](file://server/ChatServer/src/CServer.cpp#L1-L133)
- [CSession.cpp:1-335](file://server/ChatServer/src/CSession.cpp#L1-L335)

## 依赖关系分析
- CSession依赖：boost::asio、MsgNode、const、gRPC生成的pb头。
- LogicSystem依赖：Singleton、queue、thread、CSession、nlohmann/json、data、UserMgr、MysqlMgr、RedisMgr、ChatGrpcClient、DistLock。
- UserMgr依赖：CSession、RedisMgr。
- DistLock依赖：hiredis。
- 外部服务：MySQL、Redis、gRPC对端ChatServer实例。

```mermaid
graph LR
CSession["CSession"] --> MsgNode["MsgNode"]
CSession --> Const["const.h"]
CSession --> PB["chat.pb.h"]
Logic["LogicSystem"] --> CSession
Logic --> UserMgr
Logic --> MysqlMgr
Logic --> RedisMgr
Logic --> ChatGrpcClient
Logic --> DistLock
UserMgr --> CSession
DistLock --> RedisMgr
```

图表来源 
- [CSession.h:1-86](file://server/ChatServer/include/CSession.h#L1-L86)
- [LogicSystem.h:1-60](file://server/ChatServer/include/LogicSystem.h#L1-L60)
- [UserMgr.h:1-22](file://server/ChatServer/include/UserMgr.h#L1-L22)
- [DistLock.h:1-18](file://server/ChatServer/include/DistLock.h#L1-L18)

章节来源
- [CSession.h:1-86](file://server/ChatServer/include/CSession.h#L1-L86)
- [LogicSystem.h:1-60](file://server/ChatServer/include/LogicSystem.h#L1-L60)
- [UserMgr.h:1-22](file://server/ChatServer/include/UserMgr.h#L1-L22)
- [DistLock.h:1-18](file://server/ChatServer/include/DistLock.h#L1-L18)

## 性能考虑
- IO模型：基于Boost.Asio的异步非阻塞IO，配合AsioIOServicePool提升并发能力。
- 粘包处理：固定头部+变长体，减少解析开销；发送队列限流（MAX_SENDQUE）避免内存膨胀。
- 心跳机制：服务端侧定期扫描，客户端侧主动心跳，降低僵尸连接占用。
- 缓存优先：用户基础信息与好友列表优先查Redis，未命中再落库，降低DB压力。
- 跨服通知：仅当接收方不在本服时才发起gRPC调用，减少不必要的远程调用。
- 分布式锁：短超时+Lua原子释放，避免死锁与长时间持有。

[本节为通用指导，无需特定文件来源]

## 故障排查指南
- 连接异常：查看CSession::HandleWrite与AsyncRead系列错误分支，确认网络错误与长度不匹配日志。
- 心跳超时：检查CSession::_last_heartbeat更新与CServer::on_timer扫描逻辑，确认客户端心跳频率。
- 登录失败：核对Redis中Token键值、UID有效性、分布式锁竞争情况。
- 跨服通知失败：检查ChatGrpcClient调用与对端服务可用性。
- 内存泄漏：关注MsgNode分配与析构，确保发送/接收缓冲正确释放。

章节来源
- [CSession.cpp:1-335](file://server/ChatServer/src/CSession.cpp#L1-L335)
- [CServer.cpp:1-133](file://server/ChatServer/src/CServer.cpp#L1-L133)
- [LogicSystem.cpp:1-945](file://server/ChatServer/src/LogicSystem.cpp#L1-L945)

## 结论
ChatServer以CSession为核心承载TCP会话，LogicSystem作为消息总线驱动业务流转，UserMgr维护在线映射，DistLock保障多进程一致性。整体设计清晰、可扩展性强，适合大规模即时通讯场景。建议持续优化缓存命中率、监控跨服延迟、完善错误码与可观测性。

[本节为总结，无需特定文件来源]

## 附录：消息协议与错误码
- 错误码：Success、Error_Json、RPCFailed、VarifyExpired、VarifyCodeErr、UserExist、PasswdErr、EmailNotMatch、PasswdUpFailed、PasswdInvalid、TokenInvalid、UidInvalid、CREATE_CHAT_FAILED、LOAD_CHAT_FAILED。
- 消息ID：登录、搜索、好友申请/认证、文本/图片聊天、心跳、线程加载、文件同步等。
- gRPC接口：NotifyAddFriend、NotifyAuthFriend、NotifyTextChatMsg、NotifyKickUser、NotifyChatImgMsg及对应请求/响应结构。

章节来源
- [const.h:1-104](file://server/ChatServer/include/const.h#L1-L104)
- [chat.proto:1-105](file://proto/chat_service/chat.proto#L1-L105)