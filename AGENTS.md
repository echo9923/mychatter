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

## 客户端本地消息架构（本次改造新增）

- 权威存储在服务端 MySQL（`chat_message` + `user_message_sync` 创建流表，迁移脚本 `sql备份/20260813_user_message_sync.sql`，须先于服务端发布应用）
- 客户端每用户一个 SQLite：`AppDataLocation/user/<uid>/chat.db`（messages/conversations/outbox/sync_state 四表，WAL + synchronous=FULL）
- 新模块（`client/llfcchat/`，C++11）：`localmessageDTO.h`（跨线程值对象）、`localchatdb`（同步 SQL 核心，非 QObject）、`localchatstore`（独立 QThread 独占 QSQLITE 命名连接，GUI/TCP/File 线程禁止直接触碰 QSqlDatabase）、`outboxdispatcher`（1301/1503/续传/1407 重试调度，驻留 TCP 线程）、`chatsyncmanager`（bootstrap + 1405/1406 增量同步，驻留 TCP 线程）
- TcpMgr 已回归纯网络传输；旧 QSettings pending/replay/离线轮询（llfcchat-delivery.ini、旧号 1051/1052 pending 拉取、Redis offline_msg ZSET）已全部删除
- 协议破坏性变更：TCP JSON 中 message_id/thread_id/sync_seq 一律十进制字符串（1407 message_ids 也是字符串数组），两端必须同批发布；1405/1406 为 ID_SYNC_MESSAGE_REQ/RSP（按 sync_seq 游标增量同步）

## 图片与文件统一资源传输（20260814 改造）

- 图片与普通文件统一为“资源消息”：创建元数据（1503）→ 查询上传进度（1509）→ 分片上传（1507）→ 完整性校验 → 发布消息 → 分片下载（1511/1513）。分片 32KiB Base64 JSON 帧，帧格式与 MAX_FILE_LEN 维持不变。
- `chat_message` 新增 `resource_status`（0 待上传/1 就绪/2 失败过期）、`content_hash`（整文件 SHA-256）、`mime_type`；`content` 存原始文件名，服务端磁盘文件统一以 `message_id` 命名于 `bin/resource/<sender_uid>/`，进行中为 `.part` 后缀，收齐校验后原子改名。迁移脚本 `sql备份/20260814_resource_unified.sql`（清库重建，不兼容旧数据/旧磁盘文件）。
- 协议语义变更（三端同批发布）：1503/1504 通用资源创建（file_name/content_hash/mime_type，上限图片 20MB/文件 100MB，可由 [Resource] 配置覆盖）；1507/1508 按 message_id+offset+chunk_sha256 上传；1509/1510 纯进度查询（返回服务端 .part 实际长度）；旧 1043/1044 续传分支已删除（首传续传统一 1507+1509）；1511/1512 下载信息（校验请求者必须是 sender/recv）；1513/1514 按 offset 下载并随片下发 SHA-256；1505 与 gRPC `NotifyChatResourceMsg` 泛化为通用资源通知（envelope 含 resource_status/content_hash/mime_type）。
- ResourceServer 按 `message_id % worker 数` 固定路由，同一 .part 只被一个线程写；分片/整文件 SHA-256 由 `server/common` 的 `llfc::Sha256Hex/Sha256FileHex`（OpenSSL EVP，llfc_server_common_crypto）校验；重复分片幂等确认、偏移超前返回 server_offset 供客户端对齐；完成点单事务 `resource_status=1` + 双方同步行后才尝试 gRPC 通知（先 DB 后 RPC 不变式）。Redis 不再保存上传进度（真值=磁盘 .part 长度+MySQL 行），头像通道（1601-1604）维持旧 seq+MD5 协议不动。
- 清理：ResourceServer 启动时及每小时清理超时（7 天）未完成资源，标记 `resource_status=2` 并补双方同步行（客户端展示“已过期”），回收陈旧 .part 与无属主最终文件；只扫 `resource/` 目录，与头像目录 `static/` 隔离。
- 客户端：MessageTextEdit 采集时一次 32KiB 遍历预计算整文件+逐片 SHA-256；发送统一走 outbox（SEND_RESOURCE，FILE_MSG 不再被跳过）；上传恢复/断线重连一律 1509 对齐服务端偏移；下载缓存按 message_id 隔离（`cache/<message_id>/<文件名>`，写 .part 逐片校验、整文件校验后原子改名）；图片自动下载显示预览，文件用新 `FileBubble`（下载/暂停/继续/打开/另存为）；`applySyncPage` 与 `insertIncoming` 一样同事务生成 DELIVERY_ACK（离线资源消息 ACK 闭环）。

## 协议号分段规则（20260823 重编号）

- 数值唯一来源：`proto/protocol_ids.h`（`llfc_proto` 命名空间，C++11 兼容）。客户端 `global.h`、ChatServer/ResourceServer/Gate/Status 各自 `const.h`、集成测试 `im_common.h` 的本地枚举壳一律引用该头——**新增/修改协议号只改这一处**，三端必须同批发布
- 消息 ID（48 个）：百位=功能域（10 账户/11 连接保活/12 好友/13 聊天/14 历史同步/15 资源/16 头像，17xx/18xx 预留）；**奇数=发起方**（请求或服务端通知）、**偶数=回包**；同一动作 REQ→RSP→NOTIFY 连号（仅 1206/1304/1506 为空号）
- 错误码独立 2xxx 段：**20xx 通用表**（Gate/Status/Chat 共用）+ **21xx 资源表**（ResourceServer 专属）；与消息 ID 的 1xxx 永不重叠。旧 1012~1027 错误码段已废弃（曾与消息 ID 数值冲突，如旧 1018 在两表语义不同）
- 新旧对照与完整表格见 `开发文档/消息ID速查表.md`（例：文本 1017/1018/1019→1301/1302/1303；资源创建 1035/1036→1503/1504；增量同步 1051/1052→1405/1406；心跳 1023/1024→1103/1104；头像 1031~1034→1601~1604；HTTP 重连 1055→1005）
- ChatServer `CSession` 校验 `msg_id <= 2048`，新号不得越界

## 注意事项

- 生成器固定为 `Ninja Multi-Config`，CMakeLists 中有强制检查
- 服务器使用 C++17，Qt 客户端使用 C++11
- JSON 库为 nlohmann-json，通过 vcpkg 管理
- 客户端新增链接 `Qt5::Sql`，并通过 `qt5_import_plugins(INCLUDE_BY_TYPE sqldrivers Qt5::QSQLiteDriverPlugin)` 静态导入 qsqlite 驱动
