---
kind: dependency_management
name: 多语言依赖管理：vcpkg + CMake + npm 统一管控
category: dependency_management
scope:
    - '**'
source_files:
    - vcpkg.json
    - CMakeLists.txt
    - cmake/Dependencies.cmake
    - server/VarifyServer/package.json
    - webrtc-demo/server/package.json
---

本项目采用**多工具链并行的依赖管理策略**，C++ 部分使用 vcpkg 作为包管理器，Node.js 子模块使用 npm，构建系统通过 CMake 统一协调。

### 1. C++ 依赖：vcpkg 清单驱动
- **声明文件**：根目录 `vcpkg.json` 集中声明所有第三方库，包括 Boost（asio/beast/date-time/filesystem/property-tree/uuid）、jsoncpp、gRPC、protobuf、hiredis、mysql-connector-cpp（启用 jdbc 特性）、Qt5（base 与 imageformats，禁用默认特性以精简体积）。
- **版本锁定**：通过 `builtin-baseline` 指向 vcpkg 仓库的特定提交哈希，确保跨环境可复现安装。
- **安装位置**：强制将 `VCPKG_INSTALLED_DIR` 设为 `./vcpkg_installed`，避免污染系统路径。
- **查找逻辑**：`cmake/Dependencies.cmake` 中封装 `llfc_find_server_dependencies()` 和 `llfc_find_client_dependencies()`，分别对服务端（Boost/gRPC/MySQL/Redis）和客户端（Qt5 Core/Gui/Network/Widgets）执行 `find_package`。
- **MSVC 运行时**：通过 `llfc_apply_msvc_defaults` 统一设置 `/utf-8 /wd4819` 编译选项及 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` 链接参数。

### 2. Node.js 依赖：npm 包管理
- **验证服务**：`server/VarifyServer/package.json` 依赖 `@grpc/grpc-js`、`ioredis`、`nodemailer`、`uuid`，提供邮箱验证码分发。
- **WebRTC 信令服务器**：`webrtc-demo/server/package.json` 依赖 `express` 与 `ws`，实现 SDP/ICE 中继协商。
- 两个子模块各自维护独立的 `package-lock.json`，保证依赖树锁定。

### 3. CMake 集成与约定
- 根 `CMakeLists.txt` 强制要求 `Ninja Multi-Config` 生成器且禁止 in-source 构建。
- 通过 `set(VCPKG_MANIFEST_DIR ...)` 在 standalone 入口自动发现 `vcpkg.json`。
- `add_subdirectory(server)` 与 `add_subdirectory(client/llfcchat)` 聚合所有子项目，统一由顶层 CMake 编排。
- 输出目录统一规约到 `out/run/<CONFIG>/<subdir>`，便于区分不同服务的二进制产物。

### 开发者规范
- 新增 C++ 依赖必须同步更新 `vcpkg.json`，不得直接修改 `vcpkg_installed` 下的内容。
- Qt 相关依赖应通过 `default-features: false` 按需启用特性，减少打包体积。
- Node.js 子模块独立维护 `package.json`，不与其他模块共享依赖。
- 所有 find_package 调用需封装在 `Dependencies.cmake` 对应函数中，保持服务端/客户端依赖解耦。