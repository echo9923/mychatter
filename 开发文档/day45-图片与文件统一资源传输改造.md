# 图片与文件统一资源传输

## 目标

图片和普通文件统一为资源消息。ChatServer 保存消息与资源元数据，ResourceServer 管理分片和磁盘文件；只有完整性校验成功并发布的资源才进入接收者 `user_events`。

## 协议

| ID | 语义 |
|---|---|
| 1503/1504 | 创建资源元数据：`original_file_name/file_size_bytes/sha256/mime_type/message_type` |
| 1505/1506 | 按 `message_id + offset + chunk_sha256` 上传分片 |
| 1507/1508 | 查询服务端 `.part` 实际长度和 `chat_messages.status` |
| 1509/1510 | 查询下载信息；请求者必须是私聊双方之一 |
| 1511/1512 | 按 offset 下载并返回分片 SHA-256 |
| 1701 | 资源发布后的实时用户事件通知 |
| 1405/1406 | 按 `event_seq` 补拉用户事件 |

64 位的 ID、事件序号和资源大小使用十进制字符串。旧续传分支、资源专用通知和接收方 ACK 已删除。

## 数据与可见性

- 1503 在一个事务中创建 `chat_messages(status=PENDING)` 和对应 `message_resources`，不创建 `user_events`。
- 资源元数据字段为 `original_file_name/file_size_bytes/sha256/mime_type`，不混入聊天消息正文。
- 分片只写 `bin/resource/<sender_user_id>/<message_id>.part`；上传进度真值是磁盘长度，不进 Redis 或 MySQL 字段。
- 收齐后校验整文件 SHA-256，并原子改名为最终文件。
- ResourceServer 在一个事务中把消息置为 `PUBLISHED`，递增接收者 `users.last_event_seq` 并插入 `user_events`。
- 重复完成返回原 `event_seq`，不再次分配序号。
- 上传失败或过期把消息置为 `FAILED` 并清理文件，不分配事件，也不通知接收者。
- 数据库提交后才尝试 1701；推送失败由 1405/1406 补齐。

## 服务端

ChatServer 创建资源时校验认证用户、私聊成员、消息类型、大小上限、文件名、SHA-256 和 MIME。`(sender_user_id, client_message_id)` 保证创建幂等，相同键但业务参数不同返回冲突。

ResourceServer 以 `message_id % worker_count` 固定路由，保证同一 `.part` 只由一个工作线程写。重复分片先校验已经落盘的内容；偏移超前返回 `server_offset` 让客户端重新对齐。发布后使用通用 `NotifyUserEvent` gRPC 通知目标 ChatServer。

启动时和每小时清理超过配置期限的未完成资源，只扫描 `bin/resource/`，不影响头像目录 `bin/static/`。头像 1601 至 1604 协议保持独立。

## 客户端

- 采集文件时一次遍历计算整文件和逐片 SHA-256。
- 图片和文件都通过 `SEND_RESOURCE` outbox；1504 后先用 1507 对齐偏移，再继续 1505。
- `messages` 只存聊天通用字段，资源信息写入 `message_resources`。
- 发送完成后，把归档路径写入 `message_resources.local_file_path`，与消息 `SENT` 和删除 outbox 同事务提交。
- 下载缓存按 `cache/<message_id>/<原文件名>` 隔离；先写 `.part`，逐片和整文件校验通过后原子改名。
- 图片自动下载预览；文件气泡支持下载、暂停、继续、打开和另存为。
- 1701/1406 的业务写入和 `sync_state.last_event_seq` 在同一 SQLite 事务中提交。

## 验证重点

- 创建幂等、分片幂等、错误 offset、坏分片哈希、整文件哈希失败和下载鉴权。
- `PUBLISHED`、接收事件和 `users.last_event_seq` 原子提交；重复完成不增序号。
- 同服、跨服和离线资源都能由 1701 或 1405/1406 最终到达。
- Pending/Failed 资源不会进入接收者事件流。
- 重复或乱序事件不会产生重复消息、重复未读或游标越过缺口。
