# 消息 ID 与错误码速查表

数值唯一来源是 `proto/protocol_ids.h`。客户端、ChatServer、ResourceServer、GateServer、StatusServer 和集成测试只能引用该文件，不能各自定义数值。

## 消息 ID

| ID | 符号 | 当前用途 |
|---:|---|---|
| 1001 | `MSG_GET_VARIFY_CODE` | Gate HTTP 获取验证码 |
| 1002 | `MSG_REG_USER` | Gate HTTP 注册 |
| 1003 | `MSG_RESET_PWD` | Gate HTTP 重置密码 |
| 1004 | `MSG_LOGIN_USER` | Gate HTTP 登录 |
| 1005 | `MSG_REASSIGN_CHAT` | Gate HTTP 重分配 ChatServer |
| 1101/1102 | `MSG_CHAT_LOGIN` / `MSG_CHAT_LOGIN_RSP` | ChatServer 登录；响应含 `checkpoint`、好友申请和联系人快照 |
| 1103/1104 | `MSG_HEART_BEAT_REQ` / `MSG_HEARTBEAT_RSP` | 心跳；请求为空对象，响应只有 `error` |
| 1105 | `MSG_NOTIFY_OFF_LINE` | 服务端通知当前连接被踢下线，无回包 |
| 1201/1202 | `MSG_SEARCH_USER_REQ` / `MSG_SEARCH_USER_RSP` | 搜索用户 |
| 1203/1204 | `MSG_ADD_FRIEND_REQ` / `MSG_ADD_FRIEND_RSP` | 创建好友申请，响应返回好友申请字段 |
| 1205/1206 | `MSG_HANDLE_FRIEND_REQ` / `MSG_HANDLE_FRIEND_RSP` | `action=accept/reject` 处理好友申请 |
| 1301/1302 | `MSG_TEXT_CHAT_REQ` / `MSG_TEXT_CHAT_RSP` | 文本消息持久化请求/响应 |
| 1303/1304 | `MSG_CREATE_PRIVATE_CHAT_REQ` / `MSG_CREATE_PRIVATE_CHAT_RSP` | 请求 `target_user_id`；响应 `target_user_id/thread_id` |
| 1401/1402 | `MSG_LOAD_CHAT_THREAD_REQ` / `MSG_LOAD_CHAT_THREAD_RSP` | 私聊会话摘要分页 |
| 1403/1404 | `MSG_LOAD_CHAT_MSG_REQ` / `MSG_LOAD_CHAT_MSG_RSP` | 单个私聊的历史消息分页 |
| 1405/1406 | `MSG_SYNC_USER_MESSAGE_REQ` / `MSG_SYNC_USER_MESSAGE_RSP` | 按 `event_seq` 增量同步用户事件 |
| 1501/1502 | `MSG_RESOURCE_LOGIN_REQ` / `MSG_RESOURCE_LOGIN_RSP` | ResourceServer 登录 |
| 1503/1504 | `MSG_CREATE_RESOURCE_REQ` / `MSG_CREATE_RESOURCE_RSP` | 创建图片或文件元数据，此时消息为 `Pending` |
| 1505/1506 | `MSG_RESOURCE_CHUNK_UPLOAD_REQ` / `MSG_RESOURCE_CHUNK_UPLOAD_RSP` | 上传资源分片；完成响应可带 `event_seq` |
| 1507/1508 | `MSG_RESOURCE_UPLOAD_PROGRESS_REQ` / `MSG_RESOURCE_UPLOAD_PROGRESS_RSP` | 查询服务端 `.part` 实际长度和消息状态 |
| 1509/1510 | `MSG_RESOURCE_DOWN_INFO_REQ` / `MSG_RESOURCE_DOWN_INFO_RSP` | 查询下载元数据和权限 |
| 1511/1512 | `MSG_RESOURCE_CHUNK_DOWN_REQ` / `MSG_RESOURCE_CHUNK_DOWN_RSP` | 下载资源分片 |
| 1601/1602 | `MSG_UPLOAD_HEAD_ICON_REQ` / `MSG_UPLOAD_HEAD_ICON_RSP` | 上传头像 |
| 1603/1604 | `MSG_DOWN_LOAD_FILE_REQ` / `MSG_DOWN_LOAD_FILE_RSP` | 下载头像 |
| 1701 | `MSG_NOTIFY_USER_MESSAGE` | 唯一实时用户事件通知，无回包 |

未列出的数字没有协议语义，也不表示为未来功能预留。`1105` 和 `1701` 是单向通知。

## 类型字段

聊天消息只使用 `message_type`：

| 值 | 含义 |
|---:|---|
| 0 | TEXT |
| 1 | IMAGE |
| 3 | FILE |

用户事件使用 `event_type`。聊天事件沿用 0、1、3；好友事件使用独立值：

| 值 | 含义 | 引用字段 |
|---:|---|---|
| 0 | TEXT | `message_id` |
| 1 | IMAGE | `message_id` |
| 3 | FILE | `message_id` |
| 10 | FRIEND_APPLY | `friend_request_id` |
| 11 | FRIEND_ACCEPT | `friend_request_id` |
| 12 | FRIEND_REJECT | `friend_request_id` |

好友申请 `status` 为 `0=PENDING`、`1=ACCEPTED`、`2=REJECTED`。聊天消息 `status` 为 `0=PENDING`、`1=PUBLISHED`、`2=FAILED`。两者属于不同业务表，不混入同一个消息 DTO。

## 通用错误码

| 错误码 | 符号 | 含义 |
|---:|---|---|
| 0 | `ERR_SUCCESS` | 成功 |
| 2001 | `ERR_JSON` | JSON 或字段类型非法 |
| 2002 | `ERR_RPC_FAILED` | RPC 失败 |
| 2010 | `ERR_TOKEN_INVALID` | Token 无效 |
| 2011 | `ERR_UID_INVALID` | 用户 ID 无效 |
| 2012 | `ERR_CREATE_CHAT_FAILED` | 私聊不存在或请求者不是成员 |
| 2013 | `ERR_LOAD_CHAT_FAILED` | 历史加载失败 |
| 2014 | `ERR_MESSAGE_STORE_FAILED` | 持久化失败，可重试 |
| 2015 | `ERR_RECIPIENT_OFFLINE` | 接收者离线 |
| 2016 | `ERR_SERVER_BUSY` | 服务繁忙，可重试 |
| 2017 | `ERR_MESSAGE_CONFLICT` | 相同客户端幂等键对应不同内容 |
| 2018 | `ERR_NO_CHAT_SERVER` | 无可用 ChatServer |
| 2019 | `ERR_RESOURCE_INVALID` | 资源元数据非法 |
| 2020 | `ERR_RESOURCE_SIZE_EXCEEDED` | 资源超过上限 |
| 2021 | `ERR_FRIEND_REQUEST_NOT_FOUND` | 好友申请不存在或无权处理 |
| 2022 | `ERR_FRIEND_REQUEST_HANDLED` | 申请已被相反动作处理 |
| 2023 | `ERR_ALREADY_FRIENDS` | 双方已经是好友 |
| 2024 | `ERR_FRIEND_ACTION_INVALID` | `action` 不是 `accept/reject` |
| 2025 | `ERR_SYNC_CURSOR_INVALID` | 客户端 `event_seq` 游标领先服务端序号头 |

资源错误码保持 `2101` 至 `2116`，定义见 `proto/protocol_ids.h`。

## 传输约束

- 64 位的 `message_id`、`friend_request_id`、`thread_id`、`event_seq` 和 `file_size_bytes` 使用 JSON 十进制字符串，禁止 JSON number。
- `1301` 的发送者、`1405` 的事件接收者和资源操作人只能取认证 Session，不接受客户端自报身份。
- `1701` 不存在响应包；可靠性由连续 `event_seq`、缺口补拉和周期同步保证。
- 本次协议不兼容旧版本，客户端、ChatServer 和 ResourceServer 必须同批发布。
