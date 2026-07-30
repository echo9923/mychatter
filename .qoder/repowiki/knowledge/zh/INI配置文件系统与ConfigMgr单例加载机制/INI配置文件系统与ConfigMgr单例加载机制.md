---
kind: configuration_system
name: INI配置文件系统与ConfigMgr单例加载机制
category: configuration_system
scope:
    - '**'
source_files:
    - server/GateServer/include/ConfigMgr.h
    - server/GateServer/src/ConfigMgr.cpp
    - server/ChatServer/include/ConfigMgr.h
    - server/ChatServer/src/ConfigMgr.cpp
    - server/ResourceServer/include/ConfigMgr.h
    - server/ResourceServer/src/ConfigMgr.cpp
    - server/StatusServer/include/ConfigMgr.h
    - server/StatusServer/src/ConfigMgr.cpp
    - server/GateServer/config/config.ini
    - server/ChatServer/config/chatserver1.ini
    - server/ResourceServer/config/config.ini
    - server/StatusServer/config/config.ini
    - client/llfcchat/config/config.ini
    - server/VarifyServer/config.json
---

## 系统概述

该项目采用基于 INI 文件格式的轻量级配置系统，通过 Boost.PropertyTree 解析配置文件，并由每个服务模块独立的 ConfigMgr 单例类统一管理配置读取。客户端使用 Qt 标准 ini 格式，服务端各 C++ 微服务（Gate/Chat/Resource/Status）各自维护一份 config.ini，Node.js 验证服务（VarifyServer）则使用 JSON 格式。

## 核心架构与组件

### 1. C++ 服务端 ConfigMgr 单例
每个服务模块都实现了相同的 ConfigMgr 类（位于 `include/ConfigMgr.h` 和 `src/ConfigMgr.cpp`），采用单例模式（`Inst()` 静态方法）提供全局访问：
- 启动时从当前工作目录自动加载 `config.ini`
- 使用 `boost::property_tree::read_ini` 解析 INI 文件
- 将 section-key-value 结构存储在 `std::map<std::string, SectionInfo>` 中
- 提供 `GetValue(section, key)` 和 `operator[](section)[key]` 两种访问方式
- 缺失键值时返回空字符串，无类型转换或默认值处理

### 2. 配置文件组织结构
每个服务在自身目录下维护独立的 `config/config.ini` 文件：
- **GateServer**: 定义端口、下游服务地址（Verify/Status/ResServer）、数据库连接
- **ChatServer**: 除基础配置外，还包含多实例 PeerServer 配置（chatserver1/chatserver2）
- **ResourceServer**: 额外包含 Output/Static 路径配置，并自动创建输出目录
- **StatusServer**: 集中管理聊天服务器列表（chatservers）及具体实例信息
- **客户端**: 仅包含 GateServer 的连接地址

### 3. Node.js 验证服务配置
VarifyServer 使用 `config.json` 存储邮箱、MySQL、Redis 连接信息，通过 JavaScript 直接读取 JSON 文件。

## 配置约定与规范

### 标准 Section 命名
- `[SelfServer]`: 服务自身信息（Name/Host/Port/RPCPort）
- `[GateServer]`: 网关服务配置
- `[Mysql]`: 数据库连接（Host/Port/User/Passwd/Schema）
- `[Redis]`: Redis 缓存连接（Host/Port/Passwd）
- `[VarifyServer]`/`[StatusServer]`: 下游 gRPC 服务地址
- `[PeerServer]`/`[chatserverX]`: 分布式聊天服务器集群配置
- `[Output]`/`[Static]`: 资源文件路径（ResourceServer 专用）

### 路径解析策略
ResourceServer 的 ConfigMgr 扩展了路径管理功能：
- 从 `Output.Path` 和 `Static.Path` 组合生成静态资源路径
- 自动检测并创建不存在的目录
- 提供 `GetFileOutPath()` 和 `_bin_path` 成员变量供业务层使用

## 开发者注意事项

1. **配置文件位置固定**: 必须放在服务可执行文件所在目录的 `config/config.ini`，不支持命令行参数指定
2. **键名大小写敏感**: INI 解析保持原始大小写，需严格匹配
3. **无类型安全**: 所有值以字符串形式存储，需要调用方自行转换
4. **无环境变量覆盖**: 不支持通过环境变量覆盖配置项
5. **无配置热重载**: 修改配置文件后需重启服务
6. **密码明文存储**: 所有敏感信息（数据库密码、Redis 密码）以明文形式保存在配置文件中
7. **跨服务一致性**: 新增服务时需复制现有 ConfigMgr 实现并调整 section 名称