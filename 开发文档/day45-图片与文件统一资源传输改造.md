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
- 分片只写 `bin/resource/<sender_user_id>/<message_id>.part`（P2 起改为 `bin/resource/uploading/<message_id>.part`，发布后内容落 `bin/resource/blobs/<sha[:2]>/<sha256>`，见文末 P2 章节）；上传进度真值是磁盘长度，不进 Redis 或 MySQL 字段。
- 收齐后校验整文件 SHA-256，并原子改名为最终文件（P2 起为 no-replace 改名到内容寻址 blob，目标已存在即去重命中）。
- ResourceServer 在一个事务中把消息置为 `PUBLISHED`，递增接收者 `users.last_event_seq` 并插入 `user_events`。
- 重复完成返回原 `event_seq`，不再次分配序号。
- 上传失败或过期把消息置为 `FAILED` 并清理文件，不分配事件，也不通知接收者。
- 数据库提交后才尝试 1701；推送失败由 1405/1406 补齐。

## 服务端

ChatServer 创建资源时校验认证用户、私聊成员、消息类型、大小上限、文件名、SHA-256 和 MIME。`(sender_user_id, client_message_id)` 保证创建幂等，相同键但业务参数不同返回冲突。

ResourceServer 以 `message_id % worker_count` 固定路由，保证同一 `.part` 只由一个工作线程写。重复分片先校验已经落盘的内容；偏移超前返回 `server_offset` 让客户端重新对齐。发布后使用通用 `NotifyUserEvent` gRPC 通知目标 ChatServer。

启动时和每小时清理超过配置期限的未完成资源，只扫描 `bin/resource/`，不影响头像目录 `bin/static/`（P2 起同时回收无 Published 引用的孤儿 blob）。头像 1601 至 1604 协议保持独立。

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

## P1c 线协议 v2（三端同批发布，与 v1 不兼容）

- 常量与编解码唯一来源：`proto/resource_wire.h`（纯 C++11 标准库，服务端/客户端/测试共用）——`kResourceChunkSize=256KiB`、`kMaxFrameBody=2B前缀+8KiB meta+256KiB 分片`、`kMaxMetaLen=8KiB`；`EncodeFrame`（空 meta/超限 meta/超限 raw/总帧超限全部先校验后写 out）与 `DecodeFrame`（总帧上下界→meta 预算→raw 预算，全通过前不改输出）双向无符号校验。
- 仅 1505 请求与 1512 响应改为二进制体 `[u16 meta_len 大端][meta JSON][raw]`；raw 为分片原始字节，不再 Base64。1512 的错误分支与未鉴权拦截回包同样二进制（服务端统一出口 `SendRspFrame`/`SendRspFrameAndClose`）。其余消息（1501/1502/1506/1507/1508/1509/1510 及头像 1601-1604）保持 JSON，头像仍 32KiB Base64。
- 服务端：`CSession` 帧上限改用 `kMaxFrameBody`；`kResourceUploadChunkSize=256KiB`（.part 对齐/恢复单位随之变更，1508 原样下发）；FileWorker/DownloadWorker 直接读写原始字节；分片路由、幂等、配额、发布事务不变。
- 客户端：只改 `resourcecodec.cpp`（1505 编码/1512 解码走 wire）与引擎分片常量（256KiB）；FileTcpMgr 接收帧上限改用 `kMaxFrameBody`，`config.ini` 的 `ChunkSizeKB` 键已删除——分片单位固定为 `proto/resource_wire.h` 共享 256KiB（唯一真值，不可配置，旧键一律忽略）。
- 测试：`tests/integration/im_frame.h` 新增 `BuildChunkUploadReqBody/ParseChunkDownRspBody`；`im_common.h` 共享 `kTestChunkSize`（替换全部 32768 硬编码）；新增 `resource-pipeline` 场景（4 片在途、回包有序、offset 一一对应）；`tests/resource_wire_tests.cpp` 编解码往返/边界单元测试。
- 基准：`bench_run/bench_resource.py` 为真实全链路工具——ChatServer 4B 帧 1101 鉴权 + 1503/1504 创建 Pending、ResourceServer 6B 帧 1501 鉴权 + 1507 元数据/fresh 校验、二进制 1505 逐片上传（校验回显 offset/server_offset 前进/末片 Published+event_seq）、可选 `--download` 1510/1511/1512 逐片 hash/bytes/offset/total 与整文件 SHA 校验（0 字节分片立即报错）；输出 MB/s、每片 p99、帧数；`--message-id` 可复用既有 Pending，`--file` 指定源文件。
- **升级注意**：v2 与 v1 不兼容，ResourceServer、ChatServer（1503 校验不变但需同批）、客户端必须同批发布；旧客户端对新服务端的上传/下载会在 1505/1512 解析层失败（按坏帧断连），属预期。

## P2 发送者范围秒传与内容寻址存储

实现与部署细节见 `开发文档/P2-秒传与内容寻址存储.md`，此处只记结论：

- 磁盘改为内容寻址：进行中 `<CAS根>/uploading/<message_id>.part`，已发布 `<CAS根>/blobs/<sha[:2]>/<sha256>`（根路径可配置 `[Storage] ResourceRoot`，缺省 `bin/resource`；同一根目录只允许一个 ResourceServer 进程）；blob 只有"整文件哈希 + flush + no-replace 持久化改名（POSIX 目录项 fsync）"之后才出现，不建 `resource_blobs` 表，下载路径由 `message_resources.sha256` 推出。
- 上传完成用 no-replace 改名安装 blob：目标已存在即去重命中，但"存在"不等于"可信"——须复核目标精确长度 + 整文件哈希 + flush 后**才**删 `.part` 并继续发布；复核不过则保留 `.part` 与已有 blob、回可重试 `2104`，**绝不覆盖已有 blob**。安装与发布事务提交在 `BlobInstallGcMutex` 临界区内，与清理任务的孤儿判定/unlink 互斥（秒传路径同样持锁）。
- 新增 1513/1514 秒传持有性证明：1508 命中"本发送者已有相同 sha256+大小的 Published 消息且 blob 存在且大小一致"才下发一次性挑战（16 字节随机 nonce、随机区段 ≤64KiB、TTL 120 秒、绑定下发会话），客户端回 `proof=hex(SHA256(nonce_raw16‖区段))`，服务端常量时间比对，通过后走同一个发布事务与 1701。只认发送者本人历史，不泄露他人"传过相同内容"。
- 新增错误码 `2118`：证明不符/挑战丢失或过期/服务重启 → 客户端本传输禁用 proof 并回退普通上传 + 1507 重同步；服务端在证明不符后抑制本会话后续挑战，双端闭环不成环。
- `message_resources` 新增索引 `idx_message_resources_sha(sha256, file_size_bytes)`（重建脚本已含；注释版 ALTER 仅供已有 CAS 布局的库补索引参考，旧布局升级按 P2 部署说明全量重建）。
- 清理任务额外回收 7 天前 `uploading/*.part` 与无 Published 引用的孤儿 blob，**引用判定只按 sha256 身份（不附加磁盘 `stat` 大小）**，`Transient` 查库失败一律保留；旧 `resource/<sender_uid>/` 布局不动，部署时清空整个 CAS 根目录。
