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
- 新模块（`client/llfcchat/`，C++11）：`localmessageDTO.h`（跨线程值对象）、`localchatdb`（同步 SQL 核心，非 QObject）、`localchatstore`（独立 QThread 独占 QSQLITE 命名连接，GUI/TCP/File 线程禁止直接触碰 QSqlDatabase）、`outboxdispatcher`（1017/1035/续传/1049 重试调度，驻留 TCP 线程）、`chatsyncmanager`（bootstrap + 1051/1052 增量同步，驻留 TCP 线程）
- TcpMgr 已回归纯网络传输；旧 QSettings pending/replay/离线轮询（llfcchat-delivery.ini、旧 1051/1052 pending 拉取、Redis offline_msg ZSET）已全部删除
- 协议破坏性变更：TCP JSON 中 message_id/thread_id/sync_seq 一律十进制字符串（1049 message_ids 也是字符串数组），两端必须同批发布；1051/1052 数值不变但语义改为 ID_SYNC_MESSAGE_REQ/RSP

## 注意事项

- 生成器固定为 `Ninja Multi-Config`，CMakeLists 中有强制检查
- 服务器使用 C++17，Qt 客户端使用 C++11
- JSON 库为 nlohmann-json，通过 vcpkg 管理
- 客户端新增链接 `Qt5::Sql`，并通过 `qt5_import_plugins(INCLUDE_BY_TYPE sqldrivers Qt5::QSQLiteDriverPlugin)` 静态导入 qsqlite 驱动
