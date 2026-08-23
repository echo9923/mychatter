# day45-图片与文件统一资源传输改造

## 背景与目标

旧图片链路（旧号 1035 登记 → 1037/1041/1043 按 seq+MD5 上传 → 1045/1047 按 seq 下载；现行编号见《消息ID速查表》）存在的问题：

- 服务端对 MD5 零校验，无 `.part` 临时文件与原子改名，半传文件直接暴露给下载侧；
- 旧 1045/1047 下载无权限校验，客户端可拼 `sender_id+name` 读任意文件；
- Redis 进度 key `file_upload_<name>` 以文件名为键，多用户同名文件互相覆盖；
- 客户端 FILE_MSG 被直接跳过（采集已就绪但发送被丢弃），无文件气泡；
- `applySyncPage` 不生成 DELIVERY_ACK，离线消息依赖服务端重推；
- 旧号 1045 在消息不存在时漏发响应，客户端永久挂起。

改造目标：图片与文件统一为“资源消息”，SHA-256 逐片+整文件校验、offset 语义续传、7 天过期清理。

## 协议变更（破坏性，三端同批发布）

| ID | 改造后语义 |
|---|---|
| 1503/1504（旧 1035/1036） | 创建资源消息（file_name/content_hash/mime_type/msg_type=1|3），ChatServer 校验会话归属与大小上限（图 20MB/文件 100MB，[Resource] 可配） |
| 1507/1508（旧 1037/1038） | 按 message_id/offset/chunk_sha256 上传分片；响应带 server_offset 与 resource_status |
| 1509/1510（旧 1041/1042） | 纯上传进度查询，返回服务端 .part 实际字节数（首传/续传统一入口） |
| 旧 1043/1044 | **删除**（旧续传分支，现行首传/续传统一 1507+1509） |
| 1511/1512（旧 1045/1046） | 下载信息查询，校验请求者必须是 sender/recv |
| 1513/1514（旧 1047/1048） | 按 offset 下载分片，响应携带该片 SHA-256 |
| 1505（旧 1039） | 通用资源消息通知（envelope 含 resource_status/content_hash/mime_type，msg_type 取 DB 真值） |
| 1601-1604（旧 1031-1034） | 头像通道保持不变（旧 seq+MD5 协议） |

## 数据库

`sql备份/20260814_resource_unified.sql`（幂等可重复执行，先于服务端发布）：

- `chat_message` 新增 `resource_status`（0 待上传/1 就绪/2 失败过期）、`content_hash` CHAR(64)、`mime_type` VARCHAR(128)、索引 `idx_resource_pending(resource_status, updated_at)`；
- `status` 语义收紧为纯阅读态（0 未读/1 失败/2 已读），3=UN_UPLOAD 废弃；
- **清库重建**：TRUNCATE chat_message/user_message_sync；旧按 unique_name 存储的磁盘文件作废（`bin/static/<uid>/` 可清空，头像需重传）。

## 服务端要点

- ChatServer `DealCreateResourceMsg`（1503）校验链：fromuid==session、会话成员（新 DAO `GetPrivateChatMembers`）、msg_type∈{1,3}、大小上限、文件名 sanitize、SHA-256 格式、MIME 格式 → 幂等 UPSERT（不写同步行，上传完成才可见）。
- ResourceServer 按 `message_id % FILE_WORKER_COUNT` 固定路由，同一 .part 只被一个线程写；分片写后读回校验；重复分片幂等（读回比对哈希）；偏移超前返回 server_offset 供客户端对齐。
- 完成判定由服务端按 total_size 触发（不信任客户端 last 字段）：整文件 SHA-256 通过 → `.part` 原子改名 → 单事务 resource_status=1 + 双方同步行 → gRPC `NotifyChatResourceMsg`（先 DB 后 RPC 不变式；失败仅日志，增量同步兜底）。
- SHA-256 工具 `llfc::Sha256Hex/Sha256FileHex/IsValidSha256Hex`（`server/common`，OpenSSL EVP，llfc_server_common_crypto）。
- 清理定时器：启动时+每小时，标记超时 7 天未完成资源为 Expired（补双方同步行，客户端展示“已过期”），回收陈旧 .part 与无属主文件；只扫 `bin/resource/`，与头像目录 `bin/static/` 隔离。
- Redis 不再保存上传进度（真值=磁盘 .part 长度 + MySQL 行），重启/Redis 宕机均不丢进度。

## 客户端要点

- MessageTextEdit 采集时一次 32KiB 遍历预计算整文件+逐片 SHA-256（`calculateFileSha256`）；图片上限 20MB、文件 100MB。
- FILE_MSG 接入 outbox（`SEND_RESOURCE`，metadata→uploading 两阶段），不再被跳过；1504 后先 1509 对齐服务端偏移再窗口续发 1507；断线/重启恢复统一走 1509（旧 1043 分支删除）。
- 下载缓存按 message_id 隔离：`AppDataLocation/user/<uid>/cache/<message_id>/<文件名>`，写 .part、逐片校验 chunk_sha256、整文件校验后原子改名；图片自动下载，文件等用户点击。
- 新 `FileBubble`（下载/暂停/继续/打开/另存为）；`applySyncPage` 同事务生成 DELIVERY_ACK（离线资源消息 ACK 闭环）。
- 本地库 messages 表新增 resource_status/content_hash/mime_type 列（ALTER 平滑升级），旧 SEND_IMAGE outbox 条目一次性清除。

## 测试

- `sha256_tests`：FIPS 向量 + 3MB 流式文件哈希 + hex 格式校验。
- `local_store_tests`：资源链路（payload 新字段/两阶段机/FILE_MSG）、同步页 ACK 闭环、旧条目清理。
- 集成场景（`ctest -L integration`）：resource-offline（原 image-offline 改造）、resource-create/upload/resume/idempotent/corrupt/perm/offset/expiry 共 8 个新场景，覆盖多分片一致下载、重启续传、重复片幂等、坏哈希拒绝、越界对齐、鉴权与 7 天过期清理。
