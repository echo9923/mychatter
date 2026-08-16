---
kind: configuration_system
name: 基于 INI/JSON 的进程级配置系统（Boost.PropertyTree + QSettings）
category: configuration_system
scope:
    - '**'
source_files:
    - server/common/include/ConfigMgr.h
    - server/common/src/ConfigMgr.cpp
    - client/llfcchat/config/config.ini
    - client/llfcchat/src/main.cpp
    - client/llfcchat/src/outboxdispatcher.cpp
    - server/GateServer/config/config.ini
    - server/ChatServer/config/chatserver1.ini
    - server/ResourceServer/config/config.ini
    - server/StatusServer/config/config.ini
    - server/VarifyServer/config.json
    - server/VarifyServer/config.js
---

## 1. 使用的系统与框架

- **服务端 C++**：统一通过 `server/common/include/ConfigMgr.h` 提供的单例 `ConfigMgr` 加载配置文件。底层使用 Boost.PropertyTree 的 `read_ini` 解析 `.ini` 文件，路径固定为当前工作目录下的 `config.ini`。
- **客户端 Qt**：不使用共享 ConfigMgr，而是各自用 `QSettings`（`IniFormat`）直接读取应用目录下的 `config.ini`。
- **Verify 服务（Node.js）**：使用 JSON 格式 `config.json`，由 `config.js` 通过 `fs.readFileSync` 读取并 `module.exports` 暴露给其他模块。

## 2. 关键文件与包

- `server/common/include/ConfigMgr.h` / `server/common/src/ConfigMgr.cpp`：C++ 服务端统一的 INI 配置管理器，提供 `Inst()` 单例、`operator[]` 按 section 访问、`GetValue(section, key)` 取值。
- `client/llfcchat/config/config.ini`：客户端配置，定义 `[GateServer]`（host/port）和 `[Delivery]`（重试退避参数）。
- `server/GateServer/config/config.ini`、`server/ChatServer/config/chatserver1.ini`、`server/ResourceServer/config/config.ini`、`server/StatusServer/config/config.ini`：各 C++ 后端服务的 INI 配置。
- `server/VarifyServer/config.json` + `server/VarifyServer/config.js`：Verify 服务的 JSON 配置及加载脚本。

## 3. 架构与约定

### 3.1 配置文件位置
- **服务端**：每个子服务独立维护自己的 `config/` 目录，启动时从**进程当前工作目录**查找 `config.ini`（`boost::filesystem::current_path() / "config.ini"`）。因此部署时需确保运行目录包含对应服务的 config.ini。
- **客户端**：从 `QCoreApplication::applicationDirPath()` 拼接 `config.ini`，即跟随可执行文件目录。
- **Verify 服务**：从 Node 进程当前目录读取 `config.json`。

### 3.2 配置结构约定
INI 采用“section 分组”方式组织，不同服务共享一组语义化 section 名：
- `[GateServer]` / `[SelfServer]`：服务自身监听地址与端口。
- `[Mysql]`：Host/Port/User/Passwd/Schema。
- `[Redis]`：Host/Port/Passwd。
- `[VarifyServer]` / `[StatusServer]`：gRPC 下游服务地址。
- `[ResServer]`：资源服务器地址。
- `[Concurrency]`：线程池/队列容量（如 HandlerWorkers、LogicWorkers、DeliveryWorkers）。
- `[Resource]`：图片/文件大小上限（MaxImageSize、MaxFileSize）。
- `[Delivery]`：gRPC 重试策略（RpcDeadlineMs、RpcMaxAttempts、RpcBackoffMs）。
- `[Discovery]`：服务注册心跳间隔与租约 TTL。
- `[Output]` / `[Static]`：资源输出目录与静态目录。

### 3.3 加载时机与生命周期
- `ConfigMgr` 在构造时立即加载 `config.ini`，后续通过 `ConfigMgr::Inst()` 全局访问；所有依赖配置的模块（MysqlMgr、RedisMgr、LogicSystem、ChatGrpcClient 等）在初始化阶段调用该单例获取值。
- 客户端在 `main.cpp` 启动早期读取 Gate 地址前缀，并在 `OutboxDispatcher` 中再次读取 Delivery 重试参数。
- Verify 服务在启动时同步读取 JSON 配置。

### 3.4 多实例部署
ChatServer 通过多个不同的 INI 文件（`chatserver1.ini`、`chatserver2.ini`）区分不同实例的 `SelfServer.Name`、端口、RPCPort 等，实现同机多实例部署。

## 4. 约定与约束

- **单一配置文件**：每个 C++ 进程只读一个 `config.ini`，不支持运行时热更新或配置合并；修改后需重启进程。
- **缺失键行为**：`SectionInfo::operator[]` 与 `GetValue` 在找不到 section/key 时返回空字符串，调用方需自行处理默认值（例如 `atoi` 得到 0）。
- **无环境变量覆盖**：代码中未发现对 `env` 变量的读取，所有配置必须显式写入配置文件。
- **无类型校验**：INI 全部以字符串存储，数值型配置（端口、超时、大小）在调用处手动转换（`atoi`、`toLongLong` 等），没有 schema 校验。
- **敏感信息未加密**：数据库密码、Redis 密码、邮箱授权码等明文存放在配置文件中（如 `Passwd = 123456.`、`email.pass`）。
- **客户端与服务端配置分离**：客户端不共享 `ConfigMgr`，而是直接使用 `QSettings` 读取本地 `config.ini`，两者配置项互不影响。
- **Node 侧配置导出模式**：`config.js` 将 JSON 配置解构后以 CommonJS module 形式 `module.exports`，被 `server.js`、`redis.js`、`email.js` 等模块按需引入。