# 编译指南

## 环境

| 组件 | 路径 |
|------|------|
| Visual Studio 2026 BuildTools | `C:\Program Files\Microsoft Visual Studio\18\BuildTools` |
| MSVC 工具集 | `VC\Tools\MSVC\14.51.36231` |
| CMake（VS 内置） | `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| Ninja（VS 内置） | `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| vcpkg | `C:\tools\vcpkg-a51bb4d\vcpkg.exe` |
| vcpkg 已安装包 | 项目根目录 `vcpkg_installed\x64-windows-static-md\` |

## 编译步骤

必须在 **PowerShell** 中执行（需要先加载 MSVC 环境）：

```powershell
# 1. 加载 VS 开发环境
& 'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation

# 2. 进入项目
cd D:\codeproject\cpp\llfcchat

# 3. 配置（首次或 CMakeLists 变动后）
cmake --preset windows-ninja

# 4. 编译全部服务器（Debug）
cmake --build out/build/windows-ninja --config Debug --target GateServer StatusServer ChatServer ResourceServer

# 5. 编译客户端
cmake --build out/build/windows-ninja --config Debug --target llfcchat
```

Release 构建将 `Debug` 换为 `Release`。

## 输出位置

```
out/run/<Config>/GateServer/GateServer.exe
out/run/<Config>/StatusServer/StatusServer.exe
out/run/<Config>/chatserver1/ChatServer.exe
out/run/<Config>/ResourceServer/ResourceServer.exe
```

## vcpkg 说明

- 清单模式，`vcpkg.json` 在项目根目录
- triplet: `x64-windows-static-md`
- `VCPKG_INSTALLED_DIR` 指向项目内 `vcpkg_installed/`（**已被 .gitignore 忽略，不随仓库提交**，需本机自行安装）
- CMakePresets.json 中 toolchainFile 引用 `$env{VCPKG_ROOT}`，若未设置该环境变量，CMake 仍可使用已有的 `vcpkg_installed/` 完成配置
- **首次构建或拉取本次改造后必须重新 `vcpkg install`**：`qt5-base` 新增了 `sqlite3plugin` feature（客户端本地消息库依赖 qsqlite 驱动），qt5-base 会整包重编（约 1–3 小时）；装完后应存在 `vcpkg_installed/x64-windows-static-md/plugins/sqldrivers/qsqlite[d].lib`

## 客户端与服务端数据库架构（20260830 重构）

- 不迁移旧假数据；唯一建库入口为 `sql备份/20260830_database_rebuild.sql`，执行后服务端只使用 `users/friendships/private_chats/chat_messages/message_resources/friend_requests/user_events` 七表。
- `chat_messages` 只保存文本、图片、文件；`message_resources` 是资源一对一扩展；好友申请与处理状态只在 `friend_requests`；`user_events` 只保存接收顺序和业务引用。当前无群聊、好友申请撤销和好友备注。
- 客户端每用户一个 SQLite：`AppDataLocation/user/<user_id>/chat.db`，只使用 `messages/message_resources/conversations/friend_requests/contacts/outbox/sync_state` 七表（WAL + synchronous=FULL，`user_version=5` 破坏性重建）。
- 新模块（`client/llfcchat/`，C++11）：`localmessageDTO.h`（跨线程值对象）、`localchatdb`（同步 SQL 核心，非 QObject）、`localchatstore`（独立 QThread 独占 QSQLITE 命名连接，GUI/TCP/File 线程禁止直接触碰 QSqlDatabase）、`outboxdispatcher`（驻留 TCP 线程）、`chatsyncmanager`（登录/重连/缺口/30～60 秒随机周期的 1405/1406 增量同步，驻留 TCP 线程）
- **OutboxDispatcher 为单向状态模型**：SQLite outbox 是唯一真值，内存 `_entries`（`client_message_id` 到运行时条目）只是登记簿。`enqueueSend` 事务提交后才广播；1302/1504/1506 只发起本地销账或阶段推进，结果事务提交后条目才离开登记簿。退避只在内存，不写 SQLite。服务端 1301/1503 按 `(sender_user_id,client_message_id)` 幂等。
- TcpMgr 已回归纯网络传输；旧 QSettings pending/replay/离线轮询（llfcchat-delivery.ini、旧号 1051/1052 pending 拉取、Redis offline_msg ZSET）已全部删除
- TCP JSON 中 64 位 `message_id/friend_request_id/thread_id/event_seq/after_event_seq/next_event_seq/file_size_bytes` 一律为十进制字符串；1405/1406 按接收者 `event_seq` 增量同步，唯一实时通知为 1701，客户端不回 ACK。

## 图片与文件统一资源传输（20260814 改造）

- 图片与普通文件统一为“资源消息”：创建元数据（1503）→ 查询上传进度（1507）→ 分片上传（1505）→ 完整性校验 → 发布消息 → 查询下载信息（1509）→ 分片下载（1511）。P1c 起分片为 256KiB 二进制帧：仅 1505 与 1512 改为 `[u16 meta_len 大端][meta JSON][raw]`（raw 为分片原始字节，不再 Base64），常量与编解码三端共用 `proto/resource_wire.h`（`kResourceChunkSize=256KiB`、`kMaxFrameBody=2B前缀+8KiB meta+256KiB 分片`，Encode/Decode 双向校验全部预算）；头像 1601/1603 仍为 32KiB Base64 JSON。**v2 与 v1 不兼容，服务端与客户端必须同批发布**。
- `chat_messages.status` 保存 Pending/Published/Failed；`message_resources` 保存 `original_file_name/file_size_bytes/sha256/mime_type`。**P2 起磁盘改为内容寻址存储**：进行中上传在 `<CAS根>/uploading/<message_id>.part`，已发布内容在 `<CAS根>/blobs/<sha256 前 2 位>/<sha256>`（根路径可配置：`[Storage] ResourceRoot` → `[Output] ResourceRoot` → 缺省 `<Output.Path>/resource`，**同一根目录只允许一个 ResourceServer 进程**，互斥是进程内锁）；blob 只有"整文件哈希校验通过 → flush → no-replace 持久化重命名"之后才会出现，且 POSIX 下对目标目录做目录项 fsync（`llfc::EnsureDirectoryDurable`）；**"同名文件存在"不等于"内容可信"**——去重命中必须复核目标精确长度 + 整文件哈希 + flush 才复用，不通过则保留 `.part` 与已有 blob 并回可重试 `2104`（**绝不覆盖已有 blob**，它可能正被其他已发布消息引用）；不新建 `resource_blobs` 表；下载路径由 `message_resources.sha256` 推出。孤儿 blob 只按 sha256 身份判定引用（不附加磁盘 `stat` 大小，查库失败一律保留）。旧 `<sender_uid>/<message_id>` 布局不迁移，部署时清空整个 CAS 根目录。
- 1503/1504 创建资源元数据；1505/1506 按 `message_id+offset+chunk_sha256` 上传；1507/1508 查询 `.part` 长度；1509/1510 查询下载信息；1511/1512 按偏移下载；**1513/1514 为 P2 秒传持有性证明**（1508 命中"本发送者已有相同 sha256+大小的 Published 消息且对应 blob 存在且大小一致"时下发 `mode:"proof"` 与一次性挑战 `nonce(16 字节 RAND_bytes)/offset/length≤64KiB/TTL 120s/绑定下发连接`，客户端在 File 线程只读源文件回 `proof=hex(SHA256(nonce_raw16‖区段))`，服务端先校验归属（非本人/无资源行回 `2115`，不是 2118）并消费挑战、再判 64 位 hex 后常量时间比对，通过则走同一个发布事务与 1701 通知；证明不符/挑战不存在或超 TTL/绑定连接不符回 `2118`，客户端 `proof_disabled` 回退普通上传，普通 1505 会清空本会话挑战，Published 重放返回原 `event_seq` 且不发新事件；非法连接/过期挑战**不消费**，畸形 proof 才置 `challenge_suppressed`）。资源发布后 1701 envelope 携带资源扩展字段。
- 秒传只对**发送者本人**已发布过的内容生效（判定联表过滤 `sender_user_id`，不向他人泄露"有人传过相同内容"）；判定与孤儿 blob 回收都走新增索引 `message_resources(sha256, file_size_bytes)`。
- ResourceServer 按 `message_id % worker 数` 固定路由；上传完成事务把 `chat_messages.status` 置为 Published，并为接收者分配 `event_seq`。失败/过期置 Failed，不分配事件。上传进度真值是磁盘 `.part` 长度。清理任务回收 7 天前的 `uploading/*.part`，以及无任何 Published 消息引用的孤儿 blob（判定查询与 unlink 与发布侧互斥，避免删掉正在发布的 blob；被占用则下一轮重试）。
- 客户端资源发送统一走 outbox；下载缓存按 `message_id` 隔离。`applySyncPage` 将业务投影和 `sync_state.last_event_seq` 放在同一事务，不生成 ACK outbox。

## 协议号分段规则（20260823 重编号）

- 数值唯一来源：`proto/protocol_ids.h`（`llfc_proto` 命名空间，C++11 兼容）。客户端 `global.h`、ChatServer/ResourceServer/Gate/Status 各自 `const.h`、集成测试 `im_common.h` 的本地枚举壳一律引用该头——**新增/修改协议号只改这一处**，三端必须同批发布
- 消息 ID：百位=功能域（10 账户/11 连接保活/12 好友/13 聊天/14 历史同步/15 资源/16 头像/17 统一用户消息）；**奇数=发起方**（请求或服务端通知）、**偶数=回包**；请求/响应连续成对编号，1105 和 1701 是无回包的单向通知
- 错误码独立 2xxx 段：**20xx 通用表**（Gate/Status/Chat 共用）+ **21xx 资源表**（ResourceServer 专属）；与消息 ID 的 1xxx 永不重叠。旧 1012~1027 错误码段已废弃（曾与消息 ID 数值冲突，如旧 1018 在两表语义不同）
- 完整表格见 `开发文档/消息ID速查表.md`；文本、资源和好友事件都通过 1701 实时通知，并由 1405/1406 按 `event_seq` 兜底
- ChatServer `CSession` 校验 `msg_id <= 2048`，新号不得越界

## 注意事项

- 生成器固定为 `Ninja Multi-Config`，CMakeLists 中有强制检查
- 服务器使用 C++17，Qt 客户端使用 C++11
- JSON 库为 nlohmann-json，通过 vcpkg 管理
- 客户端新增链接 `Qt5::Sql`，并通过 `qt5_import_plugins(INCLUDE_BY_TYPE sqldrivers Qt5::QSQLiteDriverPlugin)` 静态导入 qsqlite 驱动
