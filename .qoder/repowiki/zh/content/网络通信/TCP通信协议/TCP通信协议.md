# TCP通信协议

<cite>
**本文引用的文件**   
- [tcpmgr.h](file://client/llfcchat/include/tcpmgr.h)
- [tcpmgr.cpp](file://client/llfcchat/src/tcpmgr.cpp)
- [chatdialog.h](file://client/llfcchat/include/chatdialog.h)
- [chatdialog.cpp](file://client/llfcchat/src/chatdialog.cpp)
- [global.h](file://client/llfcchat/include/global.h)
- [userdata.h](file://client/llfcchat/include/userdata.h)
- [CSession.h](file://server/ChatServer/include/CSession.h)
- [CSession.cpp](file://server/ChatServer/src/CSession.cpp)
- [MsgNode.h](file://server/ChatServer/include/MsgNode.h)
- [MsgNode.cpp](file://server/ChatServer/src/MsgNode.cpp)
- [const.h](file://server/ChatServer/include/const.h)
- [data.h](file://server/ChatServer/include/data.h)
- [LogicSystem.h](file://server/ChatServer/include/LogicSystem.h)
- [LogicSystem.cpp](file://server/ChatServer/src/LogicSystem.cpp)
- [MysqlDao.h](file://server/ChatServer/include/MysqlDao.h)
- [MysqlDao.cpp](file://server/ChatServer/src/MysqlDao.cpp)
- [day35心跳逻辑.md](file://开发文档/day35心跳逻辑.md)
</cite>

## 更新摘要
**所做更改**   
- 新增待处理消息重放系统章节，详细说明两阶段队列握手机制和跨线程DTO结构
- 新增TextReplayDTO和ImageReplayDTO数据结构说明，解释Qt线程模型合规性设计
- 更新客户端TcpMgr的重放流程实现，包括slot_start_pending_replay和slot_replay_done
- 增强ChatDialog的GUI线程安全处理，确保QPixmap等GUI对象在正确线程创建
- 完善ACK确认系统和离线拉取功能的线程安全性说明

## 目录
1. [简介](#简介)
2. [项目结构](#项目结构)
3. [核心组件](#核心组件)
4. [架构总览](#架构总览)
5. [详细组件分析](#详细组件分析)
6. [依赖关系分析](#依赖关系分析)
7. [性能与优化](#性能与优化)
8. [故障排查指南](#故障排查指南)
9. [结论](#结论)
10. [附录：消息格式与状态码](#附录消息格式与状态码)

## 简介
本文件为 LLFCChat 的 TCP 通信系统提供完整的技术文档，覆盖客户端与服务端的核心实现、消息帧格式、粘包拆包、心跳机制、断线重连、异步 IO、消息路由、连接池与超时控制等。重点解析客户端 TcpManager（TcpMgr）与服务端 CSession 的设计与实现，并给出集成示例与排错建议。**最新更新**包括待处理消息重放系统的重大架构改进、收件人消息ACK确认系统、离线消息拉取功能和统一消息分发机制，显著增强了TCP通信的可靠性和用户体验。

## 项目结构
- 客户端（Qt + QtNetwork）
  - 核心模块：TcpMgr（单例），负责连接管理、发送队列、粘包拆包、消息分发与信号转发。
  - 类型定义：ReqId、错误码、聊天数据结构等位于 global.h 与 userdata.h。
- 服务端（Boost.Asio + gRPC）
  - 会话层：CSession 封装单个 TCP 连接的读写、粘包拆包、心跳更新与异常清理。
  - 消息节点：MsgNode/RecvNode/SendNode 用于头部与体组装、网络字节序转换。
  - 常量与消息ID：const.h 中统一定义 MSG_IDS、ErrorCodes 等。
  - 业务逻辑：LogicSystem 负责消息分发、ACK处理和离线消息拉取。

```mermaid
graph TB
subgraph "客户端"
UI["界面层"]
TM["TcpMgr<br/>连接/收发/路由/ACK/离线拉取/重放"]
GT["全局类型<br/>ReqId/错误码/数据模型"]
CD["ChatDialog<br/>GUI线程安全处理"]
end
subgraph "服务端"
CS["CSession<br/>异步IO/粘包/心跳"]
LS["LogicSystem<br/>消息分发/ACK处理/离线拉取"]
MN["MsgNode<br/>头/体封装"]
CT["常量/消息ID<br/>const.h"]
DB["MySQL/Redis<br/>持久化存储"]
end
UI --> TM
TM --> |TCP| CS
CS --> LS
LS --> DB
TM --> GT
TM --> CD
CS --> MN
CS --> CT
```

**图表来源**
- [tcpmgr.h:22-87](file://client/llfcchat/include/tcpmgr.h#L22-L87)
- [tcpmgr.cpp:1-137](file://client/llfcchat/src/tcpmgr.cpp#L1-L137)
- [chatdialog.h:75-103](file://client/llfcchat/include/chatdialog.h#L75-L103)
- [CSession.h:28-76](file://server/ChatServer/include/CSession.h#L28-L76)
- [CSession.cpp:1-41](file://server/ChatServer/src/CSession.cpp#L1-L41)
- [LogicSystem.h:1-262](file://server/ChatServer/include/LogicSystem.h#L1-L262)
- [MsgNode.h:1-48](file://server/ChatServer/include/MsgNode.h#L1-L48)
- [const.h:1-104](file://server/ChatServer/include/const.h#L1-L104)

## 核心组件
- 客户端 TcpMgr
  - 职责：建立/关闭连接、发送队列与并发写保护、接收缓冲与粘包拆包、按 ReqId 路由到处理器、向 UI 层发射信号。
  - **新增功能**：待处理消息重放系统（两阶段队列握手）、收件人消息ACK确认系统（_pending_ack状态管理）、离线消息拉取定时器（_offline_pull_timer）、统一消息分发机制（dispatchIncomingMessage）。
  - 关键成员：QTcpSocket、发送队列 _send_queue、当前发送块 _current_block、已发送计数 _bytes_sent、标志 _pending、接收缓冲 _buffer、消息处理器映射 _handlers。
- 服务端 CSession
  - 职责：基于 Boost.Asio 的异步读/写、固定长度头部+变长体粘包处理、心跳时间戳维护、异常连接清理与分布式锁协作。
  - 关键成员：tcp::socket、发送队列 _send_que、互斥 _send_lock、接收节点 _recv_msg_node、头部节点 _recv_head_node、用户UID、心跳时间 _last_heartbeat。
- 服务端 LogicSystem
  - **新增职责**：应用层投递ACK处理器（DealDeliveryAck）、离线消息拉取处理器（PullOfflineMsg）、统一消息信封构建（BuildMessageEnvelope）。
  - 负责消息路由、业务逻辑处理、跨服gRPC调用协调。

**章节来源**
- [tcpmgr.h:22-87](file://client/llfcchat/include/tcpmgr.h#L22-L87)
- [tcpmgr.cpp:1044-1079](file://client/llfcchat/src/tcpmgr.cpp#L1044-L1079)
- [CSession.h:28-76](file://server/ChatServer/include/CSession.h#L28-L76)
- [CSession.cpp:43-75](file://server/ChatServer/src/CSession.cpp#L43-L75)
- [LogicSystem.h:217-233](file://server/ChatServer/include/LogicSystem.h#L217-L233)

## 架构总览
下图展示客户端与服务端的端到端交互流程，包括连接建立、消息帧封装、异步读写、粘包拆包、心跳与异常处理，以及**新增的待处理消息重放系统、ACK确认和离线消息拉取机制**。

```mermaid
sequenceDiagram
participant UI as "UI层(ChatDialog)"
participant TM as "客户端TcpMgr"
participant S as "服务端CSession"
participant L as "业务逻辑(LogicSystem)"
participant DB as "数据库/缓存"
UI->>TM : "发起连接/发送消息"
TM->>S : "TCP连接建立"
S-->>TM : "connected/readyRead回调"
TM->>TM : "粘包拆包(头2+体len)"
TM->>TM : "按ReqId路由到处理器"
TM-->>UI : "发射信号(登录/聊天/通知)"
S->>S : "AsyncReadHead/Body"
S->>L : "投递消息到逻辑队列"
L->>DB : "存储消息/查询状态"
DB-->>L : "返回结果"
L-->>S : "构造响应/推送通知"
S-->>TM : "Send(带头部的JSON)"
Note over TM,UI : "新增待处理消息重放流程"
TM->>TM : "slot_start_pending_replay()"
TM->>TM : "解析pending→DTO"
TM-->>UI : "sig_replay_pending(texts, images)"
UI->>UI : "重建bubble/MsgInfo/QPixmap"
UI->>TM : "sig_replay_result(failed_unique_ids)"
TM->>TM : "删除失效项+重发保留项"
Note over TM,S : "新增ACK确认流程"
TM->>S : "1049 ACK请求(message_ids)"
S->>L : "处理ACK确认"
L->>DB : "MarkMessagesDelivered"
DB-->>L : "更新delivery_status"
L-->>S : "1050 ACK回复"
S-->>TM : "清理_pending_ack状态"
Note over TM,S : "新增离线消息拉取流程"
TM->>S : "1051 拉取离线消息(uid,after_message_id,limit)"
S->>L : "处理离线拉取"
L->>DB : "GetPendingMessages/GetMessagesByIds"
DB-->>L : "返回待投递消息"
L-->>S : "1052 统一envelope响应"
S-->>TM : "messages数组+next_message_id+has_more"
TM-->>UI : "统一分发处理消息"
```

**图表来源**
- [tcpmgr.cpp:18-55](file://client/llfcchat/src/tcpmgr.cpp#L18-L55)
- [tcpmgr.cpp:1601-1655](file://client/llfcchat/src/tcpmgr.cpp#L1601-L1655)
- [chatdialog.cpp:349-394](file://client/llfcchat/src/chatdialog.cpp#L349-L394)
- [CSession.cpp:130-191](file://server/ChatServer/src/CSession.cpp#L130-L191)
- [CSession.cpp:219-248](file://server/ChatServer/src/CSession.cpp#L219-L248)
- [LogicSystem.cpp:1154-1236](file://server/ChatServer/src/LogicSystem.cpp#L1154-L1236)
- [LogicSystem.cpp:1238-1437](file://server/ChatServer/src/LogicSystem.cpp#L1238-L1437)

## 详细组件分析

### 客户端 TcpMgr 分析
- 连接生命周期
  - 连接成功：触发 connected 信号，向上层返回连接结果。
  - 断开/错误：统一通过 disconnected/error 事件上报，上层可触发重连或提示。
- 发送流程
  - slot_send_data 将 ReqId 与 JSON 数据拼装成"2字节ID + 2字节长度 + 体"的帧。
  - 使用 _send_queue 与 _pending 保证顺序串行写入，避免半包/乱序。
  - bytesWritten 回调推进 _bytes_sent，完成则出队下一帧继续写。
- 接收与粘包拆包
  - readyRead 将所有可读数据追加到 _buffer。
  - 循环解析：先确保有足够字节解析头部（ID+Len），再校验体长度是否齐全，不足则等待。
  - 解析完成后调用 handleMsg 按 ReqId 分发给对应处理器。
- **新增：待处理消息重放系统（两阶段队列握手）**
  - TextReplayDTO/ImageReplayDTO：跨线程DTO结构，仅包含值类型字段，禁止裸指针。
  - slot_start_pending_replay()：TCP线程解析pending→DTO，emit sig_replay_pending到GUI线程。
  - ChatDialog::slot_replay_pending()：GUI线程重建bubble/MsgInfo/QPixmap，emit sig_replay_result回执。
  - slot_replay_done()：删除失效项、persistPendingRequests、重发保留项+启动扫描定时器。
  - 线程安全：QPixmap只能在GUI线程创建，ChatThreadData::_msg_unrsp_map只能在GUI线程修改。
- **新增：收件人消息ACK确认系统**
  - _pending_ui_ack：已收到、待 ChatDialog 确认 UI 插入的 message_id 集合。
  - _pending_ack：已确认 UI、待服务端 1050 回复的 message_id 映射（包含退避策略）。
  - flushPendingAcks()：批量发送1049 ACK请求，处理退避重试。
  - sig_chat_msg_processed()：UI层插入消息后触发ACK发送。
- **新增：离线消息拉取机制**
  - _offline_pull_timer：定时拉取定时器，间隔由配置 OfflinePullIntervalMs 控制。
  - slot_start_offline_pull()：启动离线拉取循环。
  - slot_offline_pull_timeout()：执行拉取逻辑，支持分页连续拉取。
  - 拉取参数：uid、after_message_id、limit（默认100条）。
- **新增：统一消息分发机制**
  - dispatchIncomingMessage()：统一处理1019/1039/1052消息，根据msg_type区分文本/图片。
  - 文本消息走sig_text_chat_msg信号，图片消息复用1039下载流程。
  - 自动进入_pending_ui_ack集合，等待UI确认后发送ACK。
- 线程模型
  - TcpThread 将 TcpMgr 移动到独立 QThread，避免阻塞 UI。

```mermaid
flowchart TD
Start(["进入slot_send_data"]) --> Pack["组装帧: ID(2B)+Len(2B)+Body"]
Pack --> Pending{"是否正在发送(_pending)?"}
Pending --> |是| Enqueue["_send_queue入队"] --> End
Pending --> |否| Write["_socket.write()"]
Write --> BytesW["bytesWritten回调"]
BytesW --> Update["_bytes_sent += n"]
Update --> Done{"是否全部发送完?"}
Done --> |否| Continue["继续发送剩余部分"] --> BytesW
Done --> |是| CheckQ{"队列是否为空?"}
CheckQ --> |是| Reset["_pending=false; 清空临时变量"] --> End
CheckQ --> |否| Deq["_send_queue出队"] --> Write
subgraph "待处理消息重放流程"
ReplayStart["StartPendingReplay()"] --> EmitSig["emit sig_start_pending_replay()"]
EmitSig --> ParsePending["TCP线程解析pending→DTO"]
ParsePending --> EmitReplay["emit sig_replay_pending(texts, images)"]
EmitReplay --> GUIRebuild["GUI线程重建bubble/MsgInfo/QPixmap"]
GUIRebuild --> EmitResult["emit sig_replay_result(failed_unique_ids)"]
EmitResult --> Cleanup["删除失效项+persistPendingRequests"]
Cleanup --> Resend["重发保留项+启动定时器"]
end
subgraph "新增ACK确认流程"
AckStart["收到消息message_id"] --> AddPending["_pending_ui_ack.insert(message_id)"]
AddPending --> UIConfirm{"UI确认插入?"}
UIConfirm --> |是| SendACK["发送1049 ACK请求"]
SendACK --> WaitRsp["等待1050回复"]
WaitRsp --> Confirm{"收到成功回复?"}
Confirm --> |是| RemovePending["_pending_ack.remove(message_id)"]
Confirm --> |否| Retry["指数退避重试"]
Retry --> WaitRsp
Confirm --> |否| KeepPending["保持待确认状态"]
end
subgraph "新增离线拉取流程"
PullStart["定时器触发"] --> CheckConn{"连接正常且uid有效?"}
CheckConn --> |是| PullReq["发送1051拉取请求"]
PullReq --> PullRsp["接收1052响应"]
PullRsp --> ProcessMsgs{"遍历messages数组"}
ProcessMsgs --> Dispatch["dispatchIncomingMessage处理"]
Dispatch --> More{"has_more=true?"}
More --> |是| NextPull["立即拉取下一页"]
More --> |否| End
CheckConn --> |否| Stop["停止拉取"]
end
```

**图表来源**
- [tcpmgr.cpp:1044-1079](file://client/llfcchat/src/tcpmgr.cpp#L1044-L1079)
- [tcpmgr.cpp:103-129](file://client/llfcchat/src/tcpmgr.cpp#L103-L129)
- [tcpmgr.cpp:1601-1655](file://client/llfcchat/src/tcpmgr.cpp#L1601-L1655)
- [tcpmgr.cpp:1795-1823](file://client/llfcchat/src/tcpmgr.cpp#L1795-L1823)
- [chatdialog.cpp:349-394](file://client/llfcchat/src/chatdialog.cpp#L349-L394)
- [tcpmgr.cpp:1194-1229](file://client/llfcchat/src/tcpmgr.cpp#L1194-L1229)
- [tcpmgr.cpp:1478-1496](file://client/llfcchat/src/tcpmgr.cpp#L1478-L1496)

**章节来源**
- [tcpmgr.cpp:18-55](file://client/llfcchat/src/tcpmgr.cpp#L18-L55)
- [tcpmgr.cpp:1044-1079](file://client/llfcchat/src/tcpmgr.cpp#L1044-L1079)
- [tcpmgr.cpp:103-129](file://client/llfcchat/src/tcpmgr.cpp#L103-L129)
- [tcpmgr.cpp:1601-1655](file://client/llfcchat/src/tcpmgr.cpp#L1601-L1655)
- [tcpmgr.cpp:1795-1823](file://client/llfcchat/src/tcpmgr.cpp#L1795-L1823)
- [tcpmgr.cpp:1194-1229](file://client/llfcchat/src/tcpmgr.cpp#L1194-L1229)
- [tcpmgr.cpp:1478-1496](file://client/llfcchat/src/tcpmgr.cpp#L1478-L1496)
- [tcpmgr.h:22-87](file://client/llfcchat/include/tcpmgr.h#L22-L87)

### 服务端 CSession 分析
- 异步IO与粘包拆包
  - AsyncReadHead：读取固定长度的头部（2字节ID + 2字节长度），进行网络字节序转换与合法性校验。
  - AsyncReadBody：根据头部长度读取完整体，拷贝到 RecvNode，更新心跳时间，投递到 LogicSystem 处理。
  - asyncReadFull/asyncReadLen：递归式读取直到达到目标长度，保证完整帧。
- 发送队列与并发写
  - Send(char*/string, msgid) 将帧压入 _send_que，若队列为空则立即异步写出；否则由 HandleWrite 在回调中依次出队写出。
  - 使用 _send_lock 保护队列操作，避免竞态。
- 心跳机制
  - 每次收到数据（头/体）时更新 _last_heartbeat。
  - IsHeartbeatExpired 判断是否超过阈值（默认20s，实际建议60s）。
  - 服务器侧定时器遍历 Session，对过期连接 Close 并清理资源。
- 异常连接清理
  - DealExceptionSession：获取分布式锁后校验 session_id 一致性，清理 Redis 中的会话、IP、Token 等信息，防止僵尸连接与异地登录冲突。

```mermaid
classDiagram
class CSession {
+GetSocket() tcp : : socket&
+Start() void
+Send(msg, msgid) void
+Close() void
+AsyncReadHead(total_len) void
+AsyncReadBody(total_len) void
+IsHeartbeatExpired(now) bool
+UpdateHeartbeat() void
+DealExceptionSession() void
-_socket : tcp : : socket
-_send_que : queue<SendNode>
-_recv_msg_node : RecvNode
-_last_heartbeat : time_t
-_session_mtx : mutex
}
class LogicSystem {
+PostMsgToQue(routing_key, msg) bool
+PostToUser(uid, task) bool
+PostDelivery(sender_uid, task) bool
+Stop() void
+_logic_workers : vector<LogicWorker>
-_delivery_workers : vector<LogicWorker>
-_fun_callbacks : map<short, FunCallBack>
}
class MsgNode {
+Clear() void
-_cur_len : short
-_total_len : short
-_data : char*
}
class RecvNode {
-_msg_id : short
}
class SendNode {
-_msg_id : short
}
CSession --> LogicSystem : "投递消息"
CSession --> MsgNode : "使用"
CSession --> RecvNode : "接收缓存"
CSession --> SendNode : "发送缓存"
```

**图表来源**
- [CSession.h:28-76](file://server/ChatServer/include/CSession.h#L28-L76)
- [LogicSystem.h:1-262](file://server/ChatServer/include/LogicSystem.h#L1-L262)
- [MsgNode.h:1-48](file://server/ChatServer/include/MsgNode.h#L1-L48)

**章节来源**
- [CSession.cpp:130-191](file://server/ChatServer/src/CSession.cpp#L130-L191)
- [CSession.cpp:219-248](file://server/ChatServer/src/CSession.cpp#L219-L248)
- [CSession.cpp:193-217](file://server/ChatServer/src/CSession.cpp#L193-L217)
- [CSession.cpp:285-333](file://server/ChatServer/src/CSession.cpp#L285-L333)
- [MsgNode.cpp:1-18](file://server/ChatServer/src/MsgNode.cpp#L1-L18)

### 服务端 LogicSystem 分析
- **新增：应用层投递ACK处理器（DealDeliveryAck）**
  - 处理1049消息：{"uid":<receiver>,"message_ids":[...]} -> 1050 {"error":0,"message_ids":[...]}
  - 严格校验：JSON对象、uid正整数且==session->GetUserId()、message_ids非空数组且每项正int。
  - GetMessagesByIds带recv_id防越权：返回行数必须与请求完全吻合。
  - MarkMessagesDelivered成功后才回Success并逐个ZREM offline_msg:<uid>。
  - 重复ACK幂等：已是delivery_status=1的行仍返回成功。
- **新增：离线消息拉取处理器（PullOfflineMsg）**
  - 处理1051消息：{"uid":<receiver>,"after_message_id":<id>,"limit":<n>} -> 1052 {"error":0,"messages":[...],"next_message_id":<id>,"has_more":<bool>}
  - 配置参数：OfflinePullBatch(默认100)、PullMaxBytes(默认30000)、OfflineTtlSeconds(默认604800)。
  - Redis ZSET cursor拉取：ZRangeByScore(after_message_id, limit+1)得候选IDs。
  - MySQL真值验证：GetMessagesByIds过滤不可投递消息（delivery_status!=Pending或PIC/UN_UPLOAD）。
  - 并集去重：deliverable_from_redis + mysql_msgs，按message_id去重升序。
  - 缺失回填：MySQL pending但Redis缺失的消息ZAdd+Expire回填。
  - 字节限制：以dump()后UTF-8 byte数为准受PullMaxBytes上限，条数limit也是上限。
  - 病态处理：首条即超PullMaxBytes时返回显式非循环错误RPCFailed。
- **新增：统一消息信封构建（BuildMessageEnvelope）**
  - 固定字段：message_id,unique_id,thread_id,fromuid,touid,msg_type,content,content_size(十进制字符串),chat_time,status。
  - content_size用std::to_string避免Qt JSON number对64位文件大小丢精度。
- 消息路由
  - RegisterCallBacks注册各MSG_IDS的处理函数，CSession将RecvNode投递到队列，由业务线程处理并回写。

**章节来源**
- [LogicSystem.cpp:1154-1236](file://server/ChatServer/src/LogicSystem.cpp#L1154-L1236)
- [LogicSystem.cpp:1238-1437](file://server/ChatServer/src/LogicSystem.cpp#L1238-L1437)
- [LogicSystem.h:217-233](file://server/ChatServer/include/LogicSystem.h#L217-L233)

### 跨线程DTO结构与线程安全性
- **TextReplayDTO和ImageReplayDTO设计**
  - 纯值类型结构，禁止携带裸指针，确保跨线程安全传递。
  - TextReplayDTO包含thread_id、fromuid、unique_ids数组、contents数组。
  - ImageReplayDTO包含thread_id、fromuid、touid、name、md5、content_size、text_or_url、unique_id。
  - 使用Q_DECLARE_METATYPE注册元类型，支持Qt信号槽跨线程传递。
- **两阶段队列握手机制**
  - 第一阶段：TCP线程解析pending→DTO，emit sig_replay_pending到GUI线程。
  - 第二阶段：GUI线程重建GUI对象，emit sig_replay_result回执给TCP线程。
  - 确保QPixmap等GUI对象只在GUI线程创建，ChatThreadData只在GUI线程修改。
- **线程边界信号设计**
  - StartPendingReplay()只emit信号，实际处理在TCP线程slot_start_pending_replay执行。
  - StartOfflinePull()只emit信号，实际处理在TCP线程slot_start_offline_pull执行。
  - 所有跨线程通信都通过queued信号槽机制，避免直接调用。

**章节来源**
- [tcpmgr.h:39-62](file://client/llfcchat/include/tcpmgr.h#L39-L62)
- [tcpmgr.cpp:200-205](file://client/llfcchat/src/tcpmgr.cpp#L200-L205)
- [tcpmgr.cpp:223-235](file://client/llfcchat/src/tcpmgr.cpp#L223-L235)
- [chatdialog.h:80-81](file://client/llfcchat/include/chatdialog.h#L80-L81)

### 消息帧格式与粘包拆包
- 帧结构
  - 头部：2字节消息ID（网络字节序）+ 2字节消息体长度（网络字节序）。
  - 体：JSON 字符串（UTF-8）。
- 客户端
  - 使用 QDataStream 设置 BigEndian 写入 ID 与 Len，随后拼接 JSON 体。
  - 接收时循环检查缓冲区是否满足头/体长度，不足则等待。
- 服务端
  - 使用 boost::asio 的 async_read_some 递归读取至 HEAD_TOTAL_LEN，再按长度读取体。
  - 发送时将 ID/Len 转为网络字节序，拼接体后一次性写出。

**章节来源**
- [tcpmgr.cpp:1044-1079](file://client/llfcchat/src/tcpmgr.cpp#L1044-L1079)
- [CSession.cpp:130-191](file://server/ChatServer/src/CSession.cpp#L130-L191)
- [MsgNode.cpp:8-17](file://server/ChatServer/src/MsgNode.cpp#L8-L17)
- [const.h:38-44](file://server/ChatServer/include/const.h#L38-L44)

### 心跳机制与断线重连
- 客户端
  - 定时发送 ID_HEART_BEAT_REQ（含 fromuid），收到 ID_HEARTBEAT_RSP 视为存活。
  - 连接断开或错误时发出 sig_connection_closed，上层可触发重连或提示。
- 服务端
  - 每次收到数据更新 _last_heartbeat；定时器周期检测 IsHeartbeatExpired，超期则 Close 并清理。
  - 异常清理使用分布式锁保证多进程安全，清理 Redis 会话/IP/Token。
- 重连策略（建议）
  - 客户端在断开后指数退避重试，失败次数上限与最大间隔可配置。
  - 重连前清理本地未确认发送队列，必要时重新登录。

**章节来源**
- [tcpmgr.cpp:584-612](file://client/llfcchat/src/tcpmgr.cpp#L584-L612)
- [tcpmgr.cpp:94-98](file://client/llfcchat/src/tcpmgr.cpp#L94-L98)
- [CSession.cpp:285-333](file://server/ChatServer/src/CSession.cpp#L285-L333)
- [day35心跳逻辑.md:272-316](file://开发文档/day35心跳逻辑.md#L272-L316)

### 消息路由与业务处理
- 客户端
  - _handlers 以 ReqId 为键，存储 lambda 处理器；handleMsg 查找并执行。
  - 典型处理器：登录、搜索、好友申请/认证、文本/图片聊天、离线通知、心跳等。
- 服务端
  - LogicSystem 注册各 MSG_IDS 的处理函数，CSession 将 RecvNode 投递到队列，由业务线程处理并回写。

**章节来源**
- [tcpmgr.cpp:182-987](file://client/llfcchat/src/tcpmgr.cpp#L182-L987)
- [CSession.cpp:117-128](file://server/ChatServer/src/CSession.cpp#L117-L128)

### 连接池与超时控制
- 服务端数据库连接池（MysqlDao）
  - 后台线程定期检测连接健康度，失败则重建连接，保证可用性。
- 客户端发送超时
  - 可在 bytesWritten 回调中增加累计超时判定，超时则触发重连或告警。
- 网络层超时
  - QTcpSocket 的 connect 超时可通过 QTimer 辅助实现；read/write 超时可由应用层计时器监控。

**章节来源**
- [MysqlDao.h:102-160](file://server/ChatServer/include/MysqlDao.h#L102-L160)

## 依赖关系分析
- 客户端
  - TcpMgr 依赖 Qt 网络模块、QJson、QThread、QQueue。
  - 业务数据模型集中在 global.h 与 userdata.h。
  - ChatDialog 依赖 TcpMgr 的信号槽接口进行跨线程通信。
- 服务端
  - CSession 依赖 Boost.Asio、gRPC、Redis 管理器、MySQL 管理器。
  - LogicSystem 依赖 MysqlDao、RedisMgr、分布式锁。
  - 常量与消息ID集中定义于 const.h，数据模型在 data.h。

```mermaid
graph LR
TM["TcpMgr"] --> QT["Qt网络/JSON/线程"]
TM --> GT["global.h/userdata.h"]
TM --> CD["ChatDialog"]
CS["CSession"] --> AS["Boost.Asio"]
CS --> GRPC["gRPC"]
CS --> REDIS["RedisMgr"]
CS --> MYSQL["MysqlMgr"]
CS --> CONST["const.h(data.h)"]
LS["LogicSystem"] --> DAO["MysqlDao"]
LS --> REDIS2["RedisMgr"]
LS --> LOCK["分布式锁"]
```

**图表来源**
- [tcpmgr.h:22-87](file://client/llfcchat/include/tcpmgr.h#L22-L87)
- [global.h:43-88](file://client/llfcchat/include/global.h#L43-88)
- [chatdialog.h:75-103](file://client/llfcchat/include/chatdialog.h#L75-L103)
- [CSession.h:28-76](file://server/ChatServer/include/CSession.h#L28-L76)
- [const.h:49-79](file://server/ChatServer/include/const.h#L49-79)
- [LogicSystem.h:1-262](file://server/ChatServer/include/LogicSystem.h#L1-L262)

**章节来源**
- [tcpmgr.h:22-87](file://client/llfcchat/include/tcpmgr.h#L22-L87)
- [global.h:43-88](file://client/llfcchat/include/global.h#L43-88)
- [chatdialog.h:75-103](file://client/llfcchat/include/chatdialog.h#L75-L103)
- [CSession.h:28-76](file://server/ChatServer/include/CSession.h#L28-L76)
- [const.h:49-79](file://server/ChatServer/include/const.h#L49-79)

## 性能与优化
- 粘包拆包
  - 客户端使用循环解析与最小化内存拷贝（mid/remove）提升吞吐。
  - 服务端采用固定缓冲与递归读取，减少分配开销。
- 发送队列
  - 客户端 _send_queue 与 _pending 保障顺序写；服务端 _send_que 配合 HandleWrite 串行输出。
- 心跳与空闲检测
  - 服务端定时器批量检测，避免频繁加锁；客户端10s心跳保持中间设备活跃。
- I/O 模型
  - 客户端基于 Qt 事件循环，服务端基于 Asio 异步 IO，均避免阻塞。
- 连接池
  - MySQL 连接池后台健康检查与自动重连，降低连接抖动影响。
- **新增：待处理消息重放优化**
  - 两阶段队列握手：避免跨线程直接访问GUI对象，确保线程安全。
  - DTO结构优化：纯值类型传递，避免指针悬空问题。
  - 批量处理：同时恢复sender pending和ACK pending，减少重启开销。
- **新增：ACK确认优化**
  - 批量ACK发送：flushPendingAcks()合并多个message_id减少网络往返。
  - 指数退避重试：_ack_retry_initial_ms配置初始退避，避免雪崩效应。
  - 幂等处理：重复ACK请求不影响正常流程。
- **新增：离线拉取优化**
  - Redis ZSET游标拉取：避免全量扫描，支持分页高效拉取。
  - 字节限制控制：PullMaxBytes防止超大消息导致响应过大。
  - 双源并集：Redis+MySQL并集去重，保证数据完整性。

## 故障排查指南
- 常见问题
  - 粘包/半包：检查头部长度字段是否正确、网络字节序转换是否一致。
  - 连接中断：查看 error/disconnected 信号，确认心跳是否按时发送。
  - 消息丢失：检查发送队列是否被清空、HandleWrite 是否出错。
  - 死锁风险：确保分布式锁与线程锁顺序一致，参考心跳文档中的改造方案。
  - **新增：待处理消息重放问题**
    - 检查sig_replay_pending和sig_replay_result信号是否正确连接。
    - 验证TextReplayDTO/ImageReplayDTO元类型是否正确注册。
    - 确认GUI线程重建逻辑是否正常执行。
  - **新增：ACK确认问题**
    - 检查_pending_ack状态是否正确清理。
    - 验证1049/1050消息ID是否正确注册。
    - 确认数据库MarkMessagesDelivered执行成功。
  - **新增：离线拉取问题**
    - 检查Redis ZSET键是否存在：offline_msg:<uid>。
    - 验证after_message_id参数是否正确递增。
    - 确认PullMaxBytes配置是否合理。
- 定位方法
  - 打印帧内容（ID/Len/Body）与发送/接收计数。
  - 观察 _pending/_bytes_sent 与 _send_queue 长度变化。
  - 服务端日志关注 read length not match、handle write failed 等。
  - **新增：待处理消息重放日志**
    - 观察slot_start_pending_replay执行时间和DTO数量。
    - 检查sig_replay_pending和sig_replay_result信号流转。
  - **新增：ACK相关日志**
    - 观察_pending_ack.size()变化。
    - 检查ACK重试次数和退避时间。
  - **新增：离线拉取日志**
    - 监控PullOfflineMsg执行时间和返回消息数量。
    - 检查Redis ZSET大小和MySQL查询性能。

**章节来源**
- [tcpmgr.cpp:64-91](file://client/llfcchat/src/tcpmgr.cpp#L64-L91)
- [tcpmgr.cpp:1601-1655](file://client/llfcchat/src/tcpmgr.cpp#L1601-L1655)
- [tcpmgr.cpp:1795-1823](file://client/llfcchat/src/tcpmgr.cpp#L1795-L1823)
- [chatdialog.cpp:349-394](file://client/llfcchat/src/chatdialog.cpp#L349-L394)
- [CSession.cpp:193-217](file://server/ChatServer/src/CSession.cpp#L193-L217)
- [day35心跳逻辑.md:367-416](file://开发文档/day35心跳逻辑.md#L367-L416)

## 结论
LLFCChat 的 TCP 通信子系统以简洁可靠的帧格式与稳定的异步 IO 为核心，结合心跳与异常清理机制，实现了高可用的长连接通信。客户端 TcpMgr 与服务端 CSession 分工明确，消息路由清晰，具备可扩展性。**最新更新**包括待处理消息重放系统的重大架构改进、收件人消息ACK确认系统、离线消息拉取功能和统一消息分发机制，显著增强了系统的可靠性和用户体验。两阶段队列握手机制确保了Qt线程模型的合规性，跨线程DTO结构避免了线程安全问题。建议在重连策略、超时控制与监控方面进一步完善，以提升整体鲁棒性与可观测性。

## 附录：消息格式与状态码
- 帧格式
  - 头部：2字节消息ID（网络字节序）+ 2字节消息体长度（网络字节序）。
  - 体：JSON 字符串，包含业务字段与 error 状态码。
- 常用消息ID（部分）
  - 登录：MSG_CHAT_LOGIN / MSG_CHAT_LOGIN_RSP
  - 搜索：ID_SEARCH_USER_REQ / ID_SEARCH_USER_RSP
  - 好友：ID_ADD_FRIEND_REQ/RSP、ID_NOTIFY_ADD_FRIEND_REQ
  - 认证：ID_AUTH_FRIEND_REQ/RSP、ID_NOTIFY_AUTH_FRIEND_REQ
  - 聊天：ID_TEXT_CHAT_MSG_REQ/RSP、ID_NOTIFY_TEXT_CHAT_MSG_REQ
  - 图片：ID_IMG_CHAT_MSG_REQ/RSP、ID_NOTIFY_IMG_CHAT_MSG_REQ
  - 心跳：ID_HEART_BEAT_REQ / ID_HEARTBEAT_RSP
  - 线程/消息加载：ID_LOAD_CHAT_THREAD_REQ/RSP、ID_CREATE_PRIVATE_CHAT_REQ/RSP、ID_LOAD_CHAT_MSG_REQ/RSP
  - **新增：ACK确认**：ID_CHAT_DELIVERY_ACK_REQ(1049) / ID_CHAT_DELIVERY_ACK_RSP(1050)
  - **新增：离线拉取**：ID_PULL_OFFLINE_MSG_REQ(1051) / ID_PULL_OFFLINE_MSG_RSP(1052)
- 错误码（节选）
  - Success=0、Error_Json=1001、RPCFailed=1002、TokenInvalid=1010、UidInvalid=1011、LOAD_CHAT_FAILED=1013 等。
  - **新增**：MESSAGE_STORE_FAILED=1014、RECIPIENT_OFFLINE=1015、SERVER_BUSY=1016、MESSAGE_CONFLICT=1017
- 消息状态
  - UN_READ=0、SEND_FAILED=1、READED=2、UN_UPLOAD=3。
- **新增：ACK确认消息格式**
  - 1049请求：{"uid":<receiver>,"message_ids":[<id1>,<id2>,...]}
  - 1050回复：{"error":<code>,"message_ids":[<id1>,<id2>,...]}
- **新增：离线拉取消息格式**
  - 1051请求：{"uid":<receiver>,"after_message_id":<id>,"limit":<n>}
  - 1052回复：{"error":<code>,"messages":[{统一envelope}], "next_message_id":<id>,"has_more":<bool>}
- **新增：跨线程DTO结构**
  - TextReplayDTO：{thread_id:int, fromuid:int, unique_ids:[string], contents:[string]}
  - ImageReplayDTO：{thread_id:int, fromuid:int, touid:int, name:string, md5:string, content_size:long, text_or_url:string, unique_id:string}

**章节来源**
- [const.h:49-79](file://server/ChatServer/include/const.h#L49-79)
- [const.h:5-20](file://server/ChatServer/include/const.h#L5-L20)
- [const.h:96-101](file://server/ChatServer/include/const.h#L96-L101)
- [const.h:158-160](file://server/ChatServer/include/const.h#L158-L160)
- [global.h:43-88](file://client/llfcchat/include/global.h#L43-88)
- [global.h:91-95](file://client/llfcchat/include/global.h#L91-L95)
- [global.h:257-262](file://client/llfcchat/include/global.h#L257-262)
- [global.h:87-91](file://client/llfcchat/include/global.h#L87-L91)
- [tcpmgr.h:39-62](file://client/llfcchat/include/tcpmgr.h#L39-L62)