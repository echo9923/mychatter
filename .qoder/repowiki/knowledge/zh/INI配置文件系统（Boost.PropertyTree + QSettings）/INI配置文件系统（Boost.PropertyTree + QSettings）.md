---
kind: configuration_system
name: INI配置文件系统（Boost.PropertyTree + QSettings）
category: configuration_system
scope:
    - '**'
source_files:
    - server/common/include/ConfigMgr.h
    - server/common/src/ConfigMgr.cpp
    - server/ResourceServer/include/ConfigMgr.h
    - server/ResourceServer/src/ConfigMgr.cpp
    - server/ChatServer/config/chatserver1.ini
    - server/GateServer/config/config.ini
    - server/ResourceServer/config/config.ini
    - server/StatusServer/config/config.ini
    - client/llfcchat/config/config.ini
    - server/VarifyServer/config.json
    - server/VarifyServer/config.js
---

## 系统与工具
- C++服务端统一使用 Boost.PropertyTree + Boost.IniParser 解析 INI 配置文件，通过单例 `ConfigMgr` 提供全局只读访问。
- Qt客户端使用 Qt 内置的 `QSettings`（IniFormat）读取同一格式的 `config.ini`。
- Node.js 验证服务（VarifyServer）使用 JSON 文件（`config.json`）+ 原生 `fs.readFileSync` 加载配置。

## 核心文件与位置
- 公共配置管理器：`server/common/include/ConfigMgr.h`、`server/common/src/ConfigMgr.cpp`
- ResourceServer 扩展版配置管理器：`server/ResourceServer/include/ConfigMgr.h`、`server/ResourceServer/src/ConfigMgr.cpp`（额外提供 `GetFileOutPath()` 和路径初始化）
- 各服务配置文件（均位于各自 `config/` 目录）：
  - `server/ChatServer/config/chatserver1.ini`、`chatserver2.ini`
  - `server/GateServer/config/config.ini`
  - `server/ResourceServer/config/config.ini`
  - `server/StatusServer/config/config.ini`
  - `client/llfcchat/config/config.ini`
  - `bench_run/gate/config.ini`、`bench_run/chat1/config.ini`、`bench_run/chat2/config.ini`、`bench_run/status/config.ini`
- Node.js 验证服务配置：`server/VarifyServer/config.json`、`server/VarifyServer/config.js`

## 架构与约定
- **单例模式**：`ConfigMgr::Inst()` 返回全局唯一实例，构造时从当前工作目录加载 `config.ini`，之后所有模块通过该单例以 `cfg["Section"]["key"]` 或 `GetValue(section, key)` 方式读取。
- **INI 结构**：每个服务在自身目录下放置独立的 `config.ini`，按功能划分 Section，如 `[GateServer]`、`[Mysql]`、`[Redis]`、`[SelfServer]`、`[Concurrency]`、`[Delivery]`、`[Discovery]`、`[Static]`、`[Output]` 等。
- **路径解析**：ResourceServer 的 ConfigMgr 额外根据 `[Output].Path` 和 `[Static].Path` 计算静态资源输出目录，并在不存在时自动创建；common 版本仅做基础 KV 读取。
- **客户端差异**：Qt 客户端通过 `QSettings(config_path, QSettings::IniFormat)` 直接读取 `config.ini`，键名大小写敏感（如 `GateServer/host`、`GateServer/port`），与服务端 `ConfigMgr` 的键名风格一致但 API 不同。
- **Node.js 服务**：独立于 C++ 体系，使用 `config.json` 并通过 `config.js` 暴露模块导出，供 `server.js`、`redis.js`、`email.js` 等模块引入。

## 约定与约束
- 配置文件必须命名为 `config.ini`，且位于进程当前工作目录下（由 `boost::filesystem::current_path() / "config.ini"` 决定）。
- 所有键值均为字符串类型，使用时需自行转换为所需类型（代码中未见强制类型转换封装）。
- 缺失的 section 或 key 会返回空字符串，不会抛出异常，调用方需自行处理默认值。
- 每个服务可拥有多份 INI 文件（如 ChatServer 的 `chatserver1.ini`、`chatserver2.ini`），通过启动参数或部署脚本切换。
- 测试集成用例（`tests/integration/im_harness.cpp`）将 `config.ini` 放在工作目录下，说明运行环境依赖该约定。
- Node.js 验证服务的 `config.json` 包含邮箱账号、MySQL、Redis 连接信息，属于明文存储，未使用环境变量或密钥管理服务。