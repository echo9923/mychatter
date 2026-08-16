---
kind: build_system
name: CMake + vcpkg 多服务构建系统（Ninja Multi-Config / gRPC 代码生成 / CTest）
category: build_system
scope:
    - '**'
source_files:
    - CMakeLists.txt
    - CMakePresets.json
    - vcpkg.json
    - cmake/Dependencies.cmake
    - cmake/GrpcCodegen.cmake
    - server/CMakeLists.txt
    - server/common/CMakeLists.txt
    - server/ChatServer/CMakeLists.txt
    - client/llfcchat/CMakeLists.txt
    - tests/CMakeLists.txt
    - bench_run/build_tests.ps1
---

## 1. 构建系统与工具链

项目使用 **CMake 3.25+** 作为统一构建入口，强制要求使用 **Ninja Multi-Config** 生成器（根 `CMakeLists.txt`、`server/CMakeLists.txt`、`client/llfcchat/CMakeLists.txt`、`server/common/CMakeLists.txt`、`server/ChatServer/CMakeLists.txt` 均在检测到非 Ninja Multi-Config 时 `message(FATAL_ERROR)`）。通过 `CMakePresets.json` 提供预置：`windows-ninja` configure preset 指定 MSVC + Ninja Multi-Config + vcpkg toolchain，并配套 `windows-debug` / `windows-release` build presets。依赖管理完全由 **vcpkg manifest** (`vcpkg.json`) 驱动，固定 baseline `a51bb4d...`，安装产物位于仓库内 `vcpkg_installed/`。

服务端与客户端共享同一套 vcpkg 依赖：Boost.Asio/Beast/date-time/filesystem/property-tree/uuid、gRPC、Protobuf、hiredis、OpenSSL、mysql-connector-cpp (JDBC)、Qt5 (base + imageformats/webp/sqlite3plugin)。

## 2. 核心文件与包

- 顶层：`CMakeLists.txt`（入口，设置 C++17、CTest、add_subdirectory）、`CMakePresets.json`、`vcpkg.json`
- 共享 cmake 模块：`cmake/Dependencies.cmake`（封装 `llfc_find_server_dependencies` / `llfc_find_client_dependencies`、`llfc_apply_msvc_defaults`、`llfc_get_repo_root`、`llfc_setup_standalone_vcpkg`、`llfc_set_runtime_outdir`）、`cmake/GrpcCodegen.cmake`（`llfc_add_proto_library` 及 `llfc_ensure_chat/status/verify_proto`）
- 服务层：`server/CMakeLists.txt` → `common/`、`GateServer/`、`StatusServer/`、`ChatServer/`、`ResourceServer/`；每个子服务均有独立 `CMakeLists.txt`，支持从自身目录独立配置
- 客户端：`client/llfcchat/CMakeLists.txt`（Qt5 GUI 应用，AUTOMOC/AUTOUIC/AUTORCC，静态导入 QWindowsIntegrationPlugin/QSQLiteDriverPlugin 等）
- 测试：`tests/CMakeLists.txt`（无第三方测试框架，自定义 main 输出 `[PASS]/[FAIL]`，注册为 CTest）
- 基准脚本：`bench_run/build_tests.ps1`（调用 VS DevShell + `cmake --preset windows-ninja -DLLFC_RUN_INTEGRATION_TESTS=ON` + `cmake --build ... --target im_integration_tests`）

## 3. 架构与约定

### 3.1 分层库结构（server/common）
`server/common/CMakeLists.txt` 将通用能力拆成多个静态库，按职责解耦：
- `llfc_server_common`：ConfigMgr、AsioIOServicePool、LogicWorker（仅 Boost.asio/filesystem/property_tree）
- `llfc_server_common_redis`：RedisConPool、DistLock（依赖 hiredis、Boost.uuid）
- `llfc_server_common_mysql`：MySqlPool（依赖 mysql-concpp）
- `llfc_server_common_crypto`：PasswordHash、Sha256（依赖 OpenSSL::Crypto）
各服务按需链接所需子库，避免全量耦合。

### 3.2 gRPC/Protobuf 代码生成
`cmake/GrpcCodegen.cmake` 的 `llfc_add_proto_library(target, proto_file)` 在 `CMAKE_CURRENT_BINARY_DIR/generated/<target>/` 下调用 `protoc` + `grpc_cpp_plugin` 生成 `.pb.cc/.h` 与 `.grpc.pb.cc/.h`，并以 STATIC 库形式暴露给消费者；`llfc_ensure_*_proto()` 宏集中声明三个 proto 源（`proto/chat_service/chat.proto`、`status_service/status.proto`、`verify_service/verify.proto`），被 ChatServer/GateServer/StatusServer 等按需调用。

### 3.3 输出布局
所有可执行目标通过 `llfc_set_runtime_outdir(target subdir)` 统一输出到 `out/run/<CONFIG>/<subdir>/`，例如 ChatServer 输出到 `out/run/<config>/chatserver1/` 和 `chatserver2/`（POST_BUILD 自动复制一份以不同 ini 启动双实例），客户端输出到 `out/run/<config>/llfcchat/`。

### 3.4 Qt 客户端构建
`client/llfcchat/CMakeLists.txt` 显式启用 AUTOMOC/AUTOUIC/AUTORCC，并通过 `qt5_import_plugins` 静态嵌入 QWindowsIntegrationPlugin、QSQLiteDriverPlugin、QGifPlugin、QJpegPlugin、QWebpPlugin、QWindowsVistaStylePlugin；POST_BUILD 将 `config/config.ini` 与 `assets/static/` 复制到可执行目录。

### 3.5 测试体系
- 单元测试：`worker_pool_tests`、`password_hash_tests`、`sha256_tests`、`local_store_tests` 始终注册，不依赖外部服务
- 集成测试：`im_integration_tests` 仅在 `LLFC_RUN_INTEGRATION_TESTS=ON` 时编译，需要 live Redis + MySQL + 已启动的服务进程；每个 scenario 单独 `add_test(NAME im_integration_<name> COMMAND im_integration_tests --scenario <name>)`，标记 LABELS `integration` 且 `RUN_SERIAL TRUE`（因绑定固定端口 18080/18090/15052）
- 测试直接编译生产源码（如 `../server/common/src/LogicWorker.cpp`、`../server/common/src/PasswordHash.cpp`、`../client/llfcchat/src/localchatdb.cpp`），复用实现而非 mock

## 4. 约定与约束

- **强制 Ninja Multi-Config**：任意子目录独立 `cmake .` 都会因生成器不符而失败，确保多配置一致
- **禁止 in-source 构建**：根与各子目录均检测 `CMAKE_SOURCE_DIR == CMAKE_BINARY_DIR` 并报错
- **C++ 标准**：服务端与 common 使用 C++17（`target_compile_features cxx_std_17`），Qt 客户端及其本地存储测试保持 C++11（显式覆盖 `CXX_STANDARD 11`）
- **MSVC 默认选项**：`llfc_apply_msvc_defaults` 统一添加 `/utf-8 /wd4819`，并将运行时库设为 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`（MD/MDd）
- **vcpkg 路径固定**：通过 `VCPKG_MANIFEST_DIR`、`VCPKG_INSTALLED_DIR`、`LLFC_REPO_ROOT` 缓存变量锁定依赖位置，支持从 repo 根或任意子目录独立配置
- **可选集成测试**：默认构建不包含需要 Redis/MySQL/gRPC 的集成测试，需显式 `-DLLFC_RUN_INTEGRATION_TESTS=ON` 开启
- **部署后处理**：各服务 POST_BUILD 自动复制配置文件（如 ChatServer 同时复制 chatserver1.ini 与 chatserver2.ini），客户端复制 config.ini 与 static 资源
- **版本信息**：`vcpkg.json` 中声明 `version-string: "1.0.0"`，但未见 CI 发布流水线或版本号自动递增逻辑