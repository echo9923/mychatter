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

- 图片与普通文件统一为“资源消息”：创建元数据（1503）→ 查询上传进度（1507）→ 分片上传（1505）→ 完整性校验 → 发布消息 → 查询下载信息（1509）→ 分片下载（1511）。分片 32KiB Base64 JSON 帧，帧格式与 MAX_FILE_LEN 维持不变。
- `chat_messages.status` 保存 Pending/Published/Failed；`message_resources` 保存 `original_file_name/file_size_bytes/sha256/mime_type`。磁盘文件以 `message_id` 命名，进行中使用 `.part` 后缀。
- 1503/1504 创建资源元数据；1505/1506 按 `message_id+offset+chunk_sha256` 上传；1507/1508 查询 `.part` 长度；1509/1510 查询下载信息；1511/1512 按偏移下载。资源发布后 1701 envelope 携带资源扩展字段。
- ResourceServer 按 `message_id % worker 数` 固定路由；上传完成事务把 `chat_messages.status` 置为 Published，并为接收者分配 `event_seq`。失败/过期置 Failed，不分配事件。上传进度真值是磁盘 `.part` 长度。
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
