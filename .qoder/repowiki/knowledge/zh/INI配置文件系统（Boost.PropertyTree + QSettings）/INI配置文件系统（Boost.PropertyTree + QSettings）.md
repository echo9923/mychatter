---
kind: configuration_system
name: INI配置文件系统（Boost.PropertyTree + QSettings）
category: configuration_system
scope:
    - '**'
source_files:
    - server/ChatServer/include/ConfigMgr.h
    - server/ChatServer/src/ConfigMgr.cpp
    - server/GateServer/include/ConfigMgr.h
    - server/GateServer/src/ConfigMgr.cpp
    - server/ChatServer/config/chatserver1.ini
    - server/GateServer/config/config.ini
    - server/ResourceServer/config/config.ini
    - server/StatusServer/config/config.ini
    - server/VarifyServer/config.json
    - client/llfcchat/config/config.ini
    - client/llfcchat/src/main.cpp
---

该项目采用统一的 INI 文件格式作为配置载体，通过 C++ 服务端使用 Boost.PropertyTree 解析、Qt 客户端使用 QSettings 读取的方式实现跨语言一致的配置管理。所有服务均遵循「进程工作目录下 config.ini」的约定路径，按 Section 组织不同模块的配置项。

**核心架构与组件**
- 服务端 ConfigMgr：每个 C++ 服务（ChatServer/GateServer/ResourceServer/StatusServer）均内置独立的 ConfigMgr 单例类，基于 `boost::property_tree::ini_parser` 在构造函数中加载当前目录下的 `config.ini`，将 Section 映射为 `SectionInfo` 对象，提供 `ConfigMgr::Inst()[section][key]` 和 `GetValue(section, key)` 两种访问方式。
- 客户端 Qt 配置：`client/llfcchat/src/main.cpp` 直接使用 `QSettings(config_path, QSettings::IniFormat)` 读取应用目录下的 `config.ini`，仅用于获取 GateServer 的连接地址（host/port），未封装独立管理器。
- Node.js 验证服务：`server/VarifyServer/config.json` 使用 JSON 格式存储邮箱、MySQL、Redis 连接信息，由 `server.js` 直接 `require('./config.json')` 加载。

**配置结构约定**
各服务的 `config.ini` 遵循统一 Section 命名规范：
- `[SelfServer]`：自身服务名称、监听 Host/Port、RPC 端口
- `[GateServer]` / `[ResServer]` / `[StatusServer]` / `[VarifyServer]`：下游服务地址
- `[Mysql]` / `[Redis]`：数据库与缓存连接参数（Host/Port/User/Passwd/Schema）
- `[PeerServer]` / `[chatservers]`：集群节点列表（逗号分隔或独立 Section）
- `[Output]` / `[Static]`：资源输出目录（ResourceServer 专用）

**设计决策与约束**
1. 配置加载时机：服务端 ConfigMgr 在构造时即完成文件解析并打印全部键值对，便于启动调试；客户端在 main 函数中按需读取。
2. 路径策略：服务端依赖「当前工作目录」+ `config.ini` 的相对路径，客户端则拼接 `applicationDirPath()`，两者均未支持命令行参数覆盖或环境变量注入。
3. 类型安全：所有值均以字符串形式存取，无类型转换或默认值机制，缺失键返回空字符串。
4. 多实例部署：ChatServer 提供 `chatserver1.ini` / `chatserver2.ini` 两个示例配置文件，通过修改 SelfServer.Name 区分节点。
5. 跨语言差异：C++ 服务统一 INI 格式，Node.js 服务使用 JSON，二者互不兼容但各自内部一致。

**开发者注意事项**
- 新增配置项需同步更新对应服务的 ConfigMgr 使用处，避免运行时空串返回。
- 敏感信息（密码等）直接明文写入 ini/json，未引入加密或外部密钥管理服务。
- 不支持热重载，修改配置后需重启服务生效。
- 客户端仅读取 GateServer 连接信息，其他运行时参数（如日志级别、UI 主题）硬编码在代码中。