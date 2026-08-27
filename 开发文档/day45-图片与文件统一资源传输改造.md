# 图片与文件统一资源传输

## 目标

图片和普通文件统一为资源消息。元数据、上传进度和文件内容由 ResourceServer
管理；接收者只会看到完整性校验成功的资源。资源消息与文本、好友通知共用
`chat_message.recv_seq`、`1701 MSG_NOTIFY_USER_MESSAGE` 和 `1405/1406` 增量同步。

## 协议

| ID | 语义 |
|---|---|
| 1503/1504 | 创建资源元数据，字段包括 `file_name`、`content_hash`、`mime_type` 和 `msg_type=1|3` |
| 1507/1508 | 按 `message_id + offset + chunk_sha256` 上传分片 |
| 1509/1510 | 查询服务端 `.part` 实际长度和资源状态 |
| 1511/1512 | 查询下载信息；请求者必须是发送者或接收者 |
| 1513/1514 | 按 offset 下载并返回分片 SHA-256 |
| 1701 | 资源就绪后的统一用户消息实时通知 |
| 1405/1406 | 按 `recv_seq` 补拉完整统一消息 |

所有 64 位 ID 和 `recv_seq` 使用十进制字符串。旧续传分支、1505 资源专用通知
和接收方 ACK 均已删除。

## 数据与可见性

- 1503 创建 `chat_message` 时写入 `resource_status=0`，此时 `recv_seq=NULL`，
  接收者同步不到该行。
- 分片只写入 `bin/resource/<sender_uid>/<message_id>.part`。Redis 不保存上传
  进度，真值是磁盘长度和 MySQL 元数据。
- 收齐后校验整文件 SHA-256，并原子改名为最终文件。
- ResourceServer 在一个数据库事务中锁定资源消息和接收者 `user` 行，递增
  `user.last_recv_seq`，再将资源置为 `READY` 并写入该 `recv_seq`。
- 重复完成直接返回原 `recv_seq`，不会再次分配序号。
- 上传失败或过期只标记 `resource_status=2` 并清理文件，不分配序号，也不通知
  接收者。
- 数据库提交后才尝试同服或跨服推送；推送失败不回滚，登录、重连、缺口检测和
  30 至 60 秒随机同步负责补齐。

## 服务端

ChatServer 创建资源时校验认证用户、会话成员、消息类型、大小上限、文件名、
SHA-256 和 MIME。`(sender_id, unique_id)` 保证创建幂等，相同键但不同业务参数
返回冲突。

ResourceServer 以 `message_id % worker_count` 固定路由，保证同一 `.part` 只由一个
工作线程写入。重复分片通过已落盘内容校验后幂等返回；偏移超前时返回
`server_offset` 让客户端重新对齐。资源 READY 后使用通用 gRPC
`NotifyUserMessage(message_id, to_uid)` 通知目标 ChatServer，由 ChatServer 回读数据库
并构造统一 envelope。

启动时和每小时清理超过配置期限的未完成资源，只扫描 `bin/resource/`，不影响
头像目录 `bin/static/`。头像 1601 至 1604 协议保持不变。

## 客户端

- 文件采集时一次遍历计算整文件和逐片 SHA-256。
- 图片和文件都通过 `SEND_RESOURCE` outbox 重试；1504 后先用 1509 对齐偏移，
  再继续发送 1507。
- 下载缓存按 `cache/<message_id>/<文件名>` 隔离，先写 `.part`，逐片和整文件
  校验通过后原子改名。
- 图片可自动下载预览；文件气泡支持下载、暂停、继续、打开和另存为。
- 收到 1701 时，连续序号直接和游标在同一 SQLite 事务落库；发现缺口立即暂存并
  发起 1405，同步页不会创建 ACK outbox。

## 验证重点

- 创建幂等、分片幂等、错误 offset、坏分片哈希、整文件哈希失败和下载鉴权。
- READY 与 `recv_seq` 原子提交，重复完成不增序号，事务失败不留下序号空洞。
- 同服、跨服和离线资源都能由 1701 或 1405/1406 最终到达。
- 过期资源不会进入接收者消息流。
- 客户端资源写入和 `last_recv_seq` 推进同事务，重复或乱序通知不会产生重复 UI。
