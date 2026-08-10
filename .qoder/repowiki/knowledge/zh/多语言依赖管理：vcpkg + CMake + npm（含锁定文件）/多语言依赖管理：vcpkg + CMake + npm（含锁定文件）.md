---
kind: dependency_management
name: 多语言依赖管理：vcpkg + CMake + npm（含锁定文件）
category: dependency_management
scope:
    - '**'
source_files:
    - vcpkg.json
    - CMakeLists.txt
    - cmake/Dependencies.cmake
    - cmake/GrpcCodegen.cmake
    - server/VarifyServer/package.json
    - server/VarifyServer/package-lock.json
    - webrtc-demo/server/package.json
    - webrtc-demo/server/package-lock.json
---

## 1. 使用的系统与工具

本仓库是一个混合技术栈项目，C++/Qt 服务端与客户端通过 **vcpkg** 进行第三方库管理，Node.js 子模块通过 **npm** 管理依赖。构建系统使用 **CMake 3.25+**，并通过自定义 `cmake/Dependencies.cmake` 统一解析依赖。

- **C++/Qt 依赖**：由根目录的 `vcpkg.json` 声明，安装到仓库内的 `vcpkg_installed/` 目录（本地 vcpkg 树），不提交二进制包。
- **Node.js 依赖**：分布在 `server/VarifyServer/package.json`（验证码服务）和 `webrtc-demo/server/package.json`（WebRTC 信令演示），均配套 `package-lock.json` 锁定版本。
- **Protobuf/gRPC 代码生成**：通过 `cmake/GrpcCodegen.cmake` 在 CMake 配置阶段调用 protoc 生成 C++ gRPC 桩代码。

## 2. 关键文件

| 文件 | 作用 |
|---|---|
| `vcpkg.json` | 声明所有 C++/Qt 第三方依赖及特性（如 `mysql-connector-cpp` 启用 `jdbc`、`qt5-imageformats` 启用 `webp`） |
| `vcpkg_installed/` | vcpkg 安装的静态库产物目录（被 `.gitignore` 忽略，但作为本地安装树存在） |
| `CMakeLists.txt`（根） | 强制使用 `Ninja Multi-Config` 生成器、禁止 in-source build，并设置 `VCPKG_MANIFEST_DIR` 与 `VCPKG_INSTALLED_DIR` |
| `cmake/Dependencies.cmake` | 集中定义 `llfc_find_server_dependencies()` / `llfc_find_client_dependencies()`，用 `find_package(... CONFIG REQUIRED)` 查找已安装的依赖 |
| `cmake/GrpcCodegen.cmake` | 封装 protobuf/gRPC 代码生成逻辑，供各服务 CMakeLists 引用 |
| `server/VarifyServer/package.json` + `package-lock.json` | 验证码 Node.js 服务依赖（@grpc/grpc-js、ioredis、nodemailer、uuid） |
| `webrtc-demo/server/package.json` + `package-lock.json` | WebRTC 演示信令服务器依赖（express、ws） |

## 3. 架构与约定

### C++/Qt 依赖链
1. 开发者在 `vcpkg.json` 中声明依赖（Boost Asio/Beast、nlohmann-json、gRPC、protobuf、hiredis、OpenSSL、mysql-connector-cpp、Qt5）。
2. 根 `CMakeLists.txt` 在 `project()` 之前强制设置 `VCPKG_MANIFEST_DIR` 指向仓库根、`VCPKG_INSTALLED_DIR` 指向 `./vcpkg_installed`，确保所有子工程共享同一安装树。
3. 每个子工程的 `CMakeLists.txt` 通过 `include(Dependencies)` 引入 `cmake/Dependencies.cmake`，再调用 `llfc_find_server_dependencies()` 或 `llfc_find_client_dependencies()` 完成 `find_package`。
4. 运行时输出统一通过 `llfc_set_runtime_outdir()` 重定向到 `out/run/<CONFIG>/<subdir>`，便于部署。

### Protobuf/gRPC 代码生成
- 通过 `cmake/GrpcCodegen.cmake` 提供的宏，在编译前根据 `.proto` 文件生成 C++ 头/源文件，再由各服务的 `CMakeLists.txt` 链接生成的目标。

### Node.js 依赖
- `server/VarifyServer/` 是独立的 npm 工程，使用 `@grpc/grpc-js` 与主 C++ gRPC 服务通信，`ioredis` 连接 Redis，`nodemailer` 发送邮件，`uuid` 生成标识。
- `webrtc-demo/server/` 是独立演示工程，使用 `express` + `ws` 提供 WebSocket 信令。
- 两者都提交了对应的 `package-lock.json`，保证可重现安装。

## 4. 约定与约束

- **必须使用 Ninja Multi-Config 生成器**：根 `CMakeLists.txt` 显式检查并在非该生成器时报 `FATAL_ERROR`。
- **禁止 in-source build**：当 `CMAKE_SOURCE_DIR == CMAKE_BINARY_DIR` 时直接报错，要求使用独立构建目录。
- **vcpkg 安装树固定于仓库内**：通过 `VCPKG_INSTALLED_DIR` 强制指向 `./vcpkg_installed`，避免污染全局环境；该目录应纳入 CI 缓存而非提交到 Git。
- **依赖版本通过 vcpkg baseline 锁定**：`vcpkg.json` 使用 `builtin-baseline` 指定 vcpkg registry 的快照哈希，确保 Boost、Qt、gRPC 等上游版本可重现。
- **Qt 最小化启用**：`qt5-base` 关闭默认特性（`default-features: false`），仅按需启用组件；`qt5-imageformats` 仅启用 `webp`。
- **MySQL Connector/C++ 启用 JDBC 特性**：以 `{ name: "mysql-connector-cpp", features: ["jdbc"] }` 形式声明。
- **Node.js 子工程各自维护 lockfile**：两个 `package-lock.json` 分别锁定对应服务的依赖树，未共享。
- **无 vendoring 策略**：C++ 依赖通过 vcpkg 下载后安装到 `vcpkg_installed/`，源码未被复制到 `vendor/` 目录；Node.js 依赖也未使用 `node_modules` 提交（lockfile 方式）。
- **无私有注册表配置**：未发现 `.npmrc`、`.netrc`、`vcpkg-configuration.json` 等私有源配置，依赖全部来自公共 vcpkg registry 与 npm 镜像（`registry.npmmirror.com` 出现在 lockfile 中）。
