# 消息 ID 与错误码速查表

数值唯一来源是 `proto/protocol_ids.h`。客户端、ChatServer、ResourceServer、GateServer、StatusServer 和集成测试只能引用该文件，不能各自定义数值。

## 消息 ID

| ID | 符号 | 方向与语义 |
|---:|---|---|
| 1001 | `MSG_GET_VARIFY_CODE` | Gate HTTP 获取验证码 |
| 1002 | `MSG_REG_USER` | Gate HTTP 注册 |
| 1003 | `MSG_RESET_PWD` | Gate HTTP 重置密码 |
| 1004 | `MSG_LOGIN_USER` | Gate HTTP 登录 |
| 1005 | `MSG_REASSIGN_CHAT` | Gate HTTP 重分配 ChatServer |
| 1101/1102 | `MSG_CHAT_LOGIN` / `MSG_CHAT_LOGIN_RSP` | ChatServer 登录；响应含 checkpoint、待处理申请和联系人快照 |
| 1103/1104 | `MSG_HEART_BEAT_REQ` / `MSG_HEARTBEAT_RSP` | 心跳 |
| 1105 | `MSG_NOTIFY_OFF_LINE` | 服务端通知被踢下线 |
| 1201/1202 | `MSG_SEARCH_USER_REQ` / `MSG_SEARCH_USER_RSP` | 搜索用户 |
| 1203/1204 | `MSG_ADD_FRIEND_REQ` / `MSG_ADD_FRIEND_RSP` | 创建好友申请；响应返回申请消息 envelope |
| 1205/1206 | `MSG_HANDLE_FRIEND_REQ` / `MSG_HANDLE_FRIEND_RSP` | `action=accept/reject` 处理申请 |
| 1301/1302 | `MSG_TEXT_CHAT_REQ` / `MSG_TEXT_CHAT_RSP` | 文本消息持久化请求/响应 |
| 1303/1304 | `MSG_CREATE_PRIVATE_CHAT_REQ` / `MSG_CREATE_PRIVATE_CHAT_RSP` | 创建私聊会话 |
| 1401/1402 | `MSG_LOAD_CHAT_THREAD_REQ` / `MSG_LOAD_CHAT_THREAD_RSP` | 会话摘要分页 |
| 1403/1404 | `MSG_LOAD_CHAT_MSG_REQ` / `MSG_LOAD_CHAT_MSG_RSP` | 会话历史分页 |
| 1405/1406 | `MSG_SYNC_USER_MESSAGE_REQ` / `MSG_SYNC_USER_MESSAGE_RSP` | 按 `recv_seq` 增量同步统一用户消息 |
| 1501/1502 | `MSG_RESOURCE_LOGIN_REQ` / `MSG_RESOURCE_LOGIN_RSP` | ResourceServer 登录 |
| 1503/1504 | `MSG_CREATE_RESOURCE_REQ` / `MSG_CREATE_RESOURCE_RSP` | 创建资源元数据；此时尚无 `recv_seq` |
| 1505/1506 | `MSG_RESOURCE_CHUNK_UPLOAD_REQ` / `MSG_RESOURCE_CHUNK_UPLOAD_RSP` | 上传分片 |
| 1507/1508 | `MSG_RESOURCE_UPLOAD_PROGRESS_REQ` / `MSG_RESOURCE_UPLOAD_PROGRESS_RSP` | 查询上传进度 |
| 1509/1510 | `MSG_RESOURCE_DOWN_INFO_REQ` / `MSG_RESOURCE_DOWN_INFO_RSP` | 查询下载信息 |
| 1511/1512 | `MSG_RESOURCE_CHUNK_DOWN_REQ` / `MSG_RESOURCE_CHUNK_DOWN_RSP` | 下载分片 |
| 1601/1602 | `MSG_UPLOAD_HEAD_ICON_REQ` / `MSG_UPLOAD_HEAD_ICON_RSP` | 上传头像 |
| 1603/1604 | `MSG_DOWN_LOAD_FILE_REQ` / `MSG_DOWN_LOAD_FILE_RSP` | 旧头像下载通道 |
| 1701 | `MSG_NOTIFY_USER_MESSAGE` | 唯一实时用户消息通知；完整 envelope；客户端不回 ACK |

当前定义表不保留已删除业务的空号。`1105` 和 `1701` 是单向通知，不存在对应响应 ID；未列出的数字没有协议语义。

## 统一消息类型

| `msg_type` | 含义 |
|---:|---|
| 0 | TEXT |
| 1 | IMAGE |
| 3 | FILE |
| 10 | FRIEND_APPLY |
| 11 | FRIEND_ACCEPT |
| 12 | FRIEND_REJECT |

`business_status`：`0=NONE`、`1=PENDING`、`2=ACCEPTED`、`3=REJECTED`。

## 通用错误码

| 错误码 | 符号 | 含义 |
|---:|---|---|
| 0 | `ERR_SUCCESS` | 成功 |
| 2001 | `ERR_JSON` | JSON 或字段类型非法 |
| 2002 | `ERR_RPC_FAILED` | RPC 失败 |
| 2010 | `ERR_TOKEN_INVALID` | Token 无效 |
| 2011 | `ERR_UID_INVALID` | UID 无效 |
| 2012 | `ERR_CREATE_CHAT_FAILED` | 会话不存在或请求者不是成员 |
| 2013 | `ERR_LOAD_CHAT_FAILED` | 历史加载失败 |
| 2014 | `ERR_MESSAGE_STORE_FAILED` | 持久化失败，可重试 |
| 2015 | `ERR_RECIPIENT_OFFLINE` | 接收者离线 |
| 2016 | `ERR_SERVER_BUSY` | 服务繁忙，可重试 |
| 2017 | `ERR_MESSAGE_CONFLICT` | 相同请求 ID 对应不同内容 |
| 2018 | `ERR_NO_CHAT_SERVER` | 无可用 ChatServer |
| 2019 | `ERR_RESOURCE_INVALID` | 资源元数据非法 |
| 2020 | `ERR_RESOURCE_SIZE_EXCEEDED` | 资源超过上限 |
| 2021 | `ERR_FRIEND_REQUEST_NOT_FOUND` | 好友申请不存在或无权处理 |
| 2022 | `ERR_FRIEND_REQUEST_HANDLED` | 申请已被相反动作处理 |
| 2023 | `ERR_ALREADY_FRIENDS` | 已经是好友 |
| 2024 | `ERR_FRIEND_ACTION_INVALID` | action 不是 accept/reject |
| 2025 | `ERR_SYNC_CURSOR_INVALID` | 客户端游标领先服务端序号头 |

资源错误码保持 `2101` 至 `2116`，定义见 `proto/protocol_ids.h`。

## 传输约束

- `message_id`、`thread_id`、`recv_seq`、`related_message_id` 和资源大小统一使用十进制字符串。
- `1405` 的 UID 只能取认证 Session，不接受客户端传 UID。
- `1701` 不存在响应包；可靠性由连续 `recv_seq`、缺口补拉和周期同步保证。
- 本次协议是破坏性升级，客户端、ChatServer 和 ResourceServer 必须同批发布。
