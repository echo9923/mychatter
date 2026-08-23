# 消息 ID 与错误码速查表

> 数值唯一来源：`proto/protocol_ids.h`（`llfc_proto` 命名空间）。各端（客户端 `global.h`、ChatServer/ResourceServer `const.h`、集成测试 `im_common.h`）的本地枚举壳一律引用该头文件，**新增/修改协议号只改这一处**。

## 编号规则（三句话）

1. **百位 = 功能域**：`10` 账户 / `11` 连接保活 / `12` 好友 / `13` 聊天 / `14` 历史同步 / `15` 资源 / `16` 头像；`17xx` 预留群聊、`18xx` 预留音视频
2. **奇数 = 发起方**（请求或服务端推送通知），**偶数 = 回包**
3. **同一动作连号**：REQ → RSP → NOTIFY（带推送的动作占 3 个号，下一动作跳到下一个奇数，故全表仅 `1206`、`1304`、`1506` 为空号）

记忆口诀：**账户 10、连接 11、好友 12、聊天 13、同步 14、资源 15、头像 16**。共 **48 个**消息 ID。

## 消息 ID 总表（48 个）

### 10xx 账户域（GateServer HTTP，仅客户端内部路由 id，不上 TCP 线）

| ID | 符号（共享头） | 含义 | 旧号 |
|----|----|----|----|
| 1001 | MSG_GET_VARIFY_CODE | 获取邮箱验证码 | 1001 |
| 1002 | MSG_REG_USER | 注册用户 | 1002 |
| 1003 | MSG_RESET_PWD | 重置密码 | 1003 |
| 1004 | MSG_LOGIN_USER | 用户登录 | 1004 |
| 1005 | MSG_REASSIGN_CHAT | 断线重连复用 token 换 ChatServer | 1055 |

### 11xx 连接与保活域（ChatServer TCP）

| ID | 符号 | 含义 | 旧号 |
|----|----|----|----|
| 1101 | MSG_CHAT_LOGIN | 登录聊天服务器（请求） | 1005 |
| 1102 | MSG_CHAT_LOGIN_RSP | 登录聊天服务器（回包） | 1006 |
| 1103 | MSG_HEART_BEAT_REQ | 心跳（请求） | 1023 |
| 1104 | MSG_HEARTBEAT_RSP | 心跳（回包） | 1024 |
| 1105 | MSG_NOTIFY_OFF_LINE | 服务端通知被踢下线（gRPC NotifyKickUser 触发） | 1021 |

### 12xx 好友域（ChatServer TCP，8 个）

| ID | 符号 | 含义 | 旧号 |
|----|----|----|----|
| 1201 | MSG_SEARCH_USER_REQ | 搜索用户（请求） | 1007 |
| 1202 | MSG_SEARCH_USER_RSP | 搜索用户（回包） | 1008 |
| 1203 | MSG_ADD_FRIEND_REQ | 添加好友（请求） | 1009 |
| 1204 | MSG_ADD_FRIEND_RSP | 添加好友（回包） | 1010 |
| 1205 | MSG_NOTIFY_ADD_FRIEND | 通知收到好友申请（NotifyAddFriend 触发） | 1011 |
| 1207 | MSG_AUTH_FRIEND_REQ | 认证好友·同意/拒绝（请求） | 1013 |
| 1208 | MSG_AUTH_FRIEND_RSP | 认证好友（回包） | 1014 |
| 1209 | MSG_NOTIFY_AUTH_FRIEND | 通知认证结果（NotifyAuthFriend 触发） | 1015 |

### 13xx 聊天域（ChatServer TCP，5 个）

| ID | 符号 | 含义 | 旧号 |
|----|----|----|----|
| 1301 | MSG_TEXT_CHAT_REQ | 发送文本消息（请求） | 1017 |
| 1302 | MSG_TEXT_CHAT_RSP | 发送文本消息（回包） | 1018 |
| 1303 | MSG_NOTIFY_TEXT_CHAT | 通知收到文本消息（NotifyTextChatMsg 触发） | 1019 |
| 1305 | MSG_CREATE_PRIVATE_CHAT_REQ | 创建私聊会话（请求） | 1027 |
| 1306 | MSG_CREATE_PRIVATE_CHAT_RSP | 创建私聊会话（回包） | 1028 |

### 14xx 历史与同步域（ChatServer TCP，8 个）

| ID | 符号 | 含义 | 旧号 |
|----|----|----|----|
| 1401 | MSG_LOAD_CHAT_THREAD_REQ | 会话列表（请求） | 1025 |
| 1402 | MSG_LOAD_CHAT_THREAD_RSP | 会话列表（回包） | 1026 |
| 1403 | MSG_LOAD_CHAT_MSG_REQ | 历史消息分页（请求） | 1029 |
| 1404 | MSG_LOAD_CHAT_MSG_RSP | 历史消息分页（回包） | 1030 |
| 1405 | MSG_SYNC_MESSAGE_REQ | 增量同步（请求，含 bootstrap 变体） | 1051 |
| 1406 | MSG_SYNC_MESSAGE_RSP | 增量同步（回包） | 1052 |
| 1407 | MSG_DELIVERY_ACK_REQ | 投递 ACK（请求，receiver 确认 message_ids） | 1049 |
| 1408 | MSG_DELIVERY_ACK_RSP | 投递 ACK（回包） | 1050 |

### 15xx 资源域（13 个；1503/1504/1505 由 ChatServer 处理，其余 ResourceServer）

| ID | 符号 | 含义 | 旧号 |
|----|----|----|----|
| 1501 | MSG_RESOURCE_LOGIN_REQ | 资源服务登录鉴权（请求，每连接一次） | 1053 |
| 1502 | MSG_RESOURCE_LOGIN_RSP | 资源服务登录鉴权（回包） | 1054 |
| 1503 | MSG_CREATE_RESOURCE_REQ | 创建资源消息·元数据先行（请求） | 1035 |
| 1504 | MSG_CREATE_RESOURCE_RSP | 创建资源消息（回包，返回 message_id） | 1036 |
| 1505 | MSG_NOTIFY_RESOURCE | 通知收到资源消息（NotifyChatResourceMsg 触发） | 1039 |
| 1507 | MSG_RESOURCE_CHUNK_UPLOAD_REQ | 分片上传（请求，offset+chunk_sha256） | 1037 |
| 1508 | MSG_RESOURCE_CHUNK_UPLOAD_RSP | 分片上传（回包，带 server_offset） | 1038 |
| 1509 | MSG_RESOURCE_UPLOAD_PROGRESS_REQ | 查询上传进度（请求，.part 真值；首传/续传统一入口） | 1041 |
| 1510 | MSG_RESOURCE_UPLOAD_PROGRESS_RSP | 查询上传进度（回包） | 1042 |
| 1511 | MSG_RESOURCE_DOWN_INFO_REQ | 查询下载信息（请求，含权限校验） | 1045 |
| 1512 | MSG_RESOURCE_DOWN_INFO_RSP | 查询下载信息（回包） | 1046 |
| 1513 | MSG_RESOURCE_CHUNK_DOWN_REQ | 分片下载（请求，按 offset） | 1047 |
| 1514 | MSG_RESOURCE_CHUNK_DOWN_RSP | 分片下载（回包，随片下发 SHA-256） | 1048 |

### 16xx 头像域（ResourceServer TCP，旧 seq+MD5 协议保留，4 个）

| ID | 符号 | 含义 | 旧号 |
|----|----|----|----|
| 1601 | MSG_UPLOAD_HEAD_ICON_REQ | 上传头像（请求） | 1031 |
| 1602 | MSG_UPLOAD_HEAD_ICON_RSP | 上传头像（回包） | 1032 |
| 1603 | MSG_DOWN_LOAD_FILE_REQ | 下载文件·旧头像链路（请求） | 1033 |
| 1604 | MSG_DOWN_LOAD_FILE_RSP | 下载文件（回包） | 1034 |

## 错误码（TCP/HTTP JSON 的 `error` 字段，独立于消息 ID）

规则：**1xxx = 消息 ID，2xxx = 错误码**，两个命名空间永不重叠。
历史上 ChatServer 与 ResourceServer 两张错误码语义表共用 1012~1027 且互相冲突（如 1018 既是"无可用节点"又是"偏移超前"），2xxx 起彻底拆分。

### 20xx 通用表（GateServer / StatusServer / ChatServer 共用，21 个）

| 码 | 符号 | 含义 | 备注 |
|----|----|----|----|
| 0 | ERR_SUCCESS | 成功 | |
| 2001 | ERR_JSON | JSON 解析错误 | |
| 2002 | ERR_RPC_FAILED | RPC 请求失败 | |
| 2003 | ERR_VARIFY_EXPIRED | 验证码已过期 | |
| 2004 | ERR_VARIFY_CODE | 验证码错误 | |
| 2005 | ERR_USER_EXIST | 用户已存在 | |
| 2006 | ERR_PASSWD | 密码错误 | |
| 2007 | ERR_EMAIL_NOT_MATCH | 邮箱不匹配 | |
| 2008 | ERR_PASSWD_UP_FAILED | 更新密码失败 | |
| 2009 | ERR_PASSWD_INVALID | 密码无效 | |
| 2010 | ERR_TOKEN_INVALID | Token 失效 | |
| 2011 | ERR_UID_INVALID | 用户 ID 无效 | |
| 2012 | ERR_CREATE_CHAT_FAILED | 创建会话失败 / 非会话成员 | |
| 2013 | ERR_LOAD_CHAT_FAILED | 加载聊天记录失败 | |
| 2014 | ERR_MESSAGE_STORE_FAILED | 消息持久化失败 | transient，继续重传 |
| 2015 | ERR_RECIPIENT_OFFLINE | 接收方离线 | |
| 2016 | ERR_SERVER_BUSY | 服务端停机/队列拒绝 | transient，继续重传 |
| 2017 | ERR_MESSAGE_CONFLICT | unique-id 冲突 | permanent，标 SEND_FAILED |
| 2018 | ERR_NO_CHAT_SERVER | 无可用 ChatServer 节点 | |
| 2019 | ERR_RESOURCE_INVALID | 资源元数据非法 | permanent |
| 2020 | ERR_RESOURCE_SIZE_EXCEEDED | 超类型上限（图 20MB/文件 100MB） | permanent |

### 21xx 资源文件表（ResourceServer 专属，16 个）

| 码 | 符号 | 含义 | 旧号 |
|----|----|----|----|
| 2101 | RS_FILE_NOT_EXISTS | 文件不存在 | 1012 |
| 2102 | RS_FILE_SAVE_REDIS_FAILED | 文件存储 Redis 失败 | 1013 |
| 2103 | RS_CREATE_FILE_PATH_FAILED | 文件路径创建失败 | 1014 |
| 2104 | RS_FILE_WRITE_PERMISSION | 文件写权限不足 | 1015 |
| 2105 | RS_FILE_READ_PERMISSION | 文件读权限不足 | 1016 |
| 2106 | RS_FILE_SEQ_INVALID | 文件序列有误（仅旧头像链路） | 1017 |
| 2107 | RS_FILE_OFFSET_INVALID | 分片偏移超前（响应带 server_offset） | 1018 |
| 2108 | RS_FILE_READ_FAILED | 文件读取失败 | 1019 |
| 2109 | RS_REDIS_READ_ERR | Redis 读取失败 | 1020 |
| 2110 | RS_SERVER_IP_ERR | server ip 错误 | 1021 |
| 2111 | RS_MSG_ID_ERR | 消息不存在（permanent，标失败） | 1022 |
| 2112 | RS_FILE_HASH_MISMATCH | 分片/整文件 SHA-256 校验失败 | 1023 |
| 2113 | RS_FILE_SIZE_EXCEEDED | 资源超过类型上限 | 1024 |
| 2114 | RS_RESOURCE_NOT_READY | 资源未就绪（稍后重试） | 1025 |
| 2115 | RS_RESOURCE_FORBIDDEN | 非收发双方（permanent） | 1026 |
| 2116 | RS_RESOURCE_STATE_INVALID | 资源已过期/终态（permanent） | 1027 |

## 各端枚举壳对照

| 端 | 文件 | 本地符号风格 |
|----|----|----|
| 客户端 | `client/llfcchat/src/protocol/global.h` | `ID_XXX = llfc_proto::MSG_XXX`（enum ReqId / enum ErrorCodes） |
| ChatServer | `server/ChatServer/include/const.h` | `MSG_CHAT_LOGIN` 等（enum MSG_TYPES / enum ErrorCodes） |
| ResourceServer | `server/ResourceServer/include/const.h` | `ID_XXX`（enum MSG_TYPES / enum ErrorCodes） |
| Gate/Status | 各自 `include/const.h` | enum ErrorCodes（引用 20xx 通用表） |
| 集成测试 | `tests/integration/im_common.h` | `imt::ID_XXX`（inline constexpr，引用共享头） |

## 发布注意

- 协议号是**破坏性变更维度**：客户端、ChatServer、ResourceServer 必须**同批发布**（旧客户端连新服务器会因未知 msg_id 被断连）
- 协议号不落库（SQLite/MySQL/QSettings 均无存储），重编号对存量数据零影响
- ChatServer `CSession` 校验 `msg_id <= 2048`，新号不得越界（当前最大 1604）
