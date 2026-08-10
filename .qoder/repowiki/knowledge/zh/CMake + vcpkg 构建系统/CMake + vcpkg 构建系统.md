---
kind: build_system
name: CMake + vcpkg 构建系统
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
    - client/llfcchat/CMakeLists.txt
    - tests/CMakeLists.txt
---

## 1. 使用的系统与工具
- **构建系统**: CMake 3.25+，强制使用 `Ninja Multi-Config` 生成器，禁止 in-source 构建。
- **依赖管理**: vcpkg（manifest 模式），通过 `vcpkg.json` 声明 Boost、gRPC、protobuf、hiredis、OpenSSL、mysql-connector-cpp、Qt5 等第三方库，安装目录固定为 `vcpkg_installed`。
- **代码生成**: 自定义 `cmake/GrpcCodegen.cmake` 封装 protoc + grpc_cpp_plugin，将 `.proto` 文件编译为静态库目标（如 `llfc_chat_proto`）。
- **测试框架**: CTest，无外部测试框架；单元测试直接链接生产源码并输出 `[PASS]/[FAIL]`，集成测试通过 `LLFC_RUN_INTEGRATION_TESTS` 选项开关。
- **预设配置**: `CMakePresets.json` 提供 Windows + MSVC + Ninja Multi-Config 的 configure/build preset。

## 2. 核心文件与包
- 根级入口: `CMakeLists.txt`、`CMakePresets.json`、`vcpkg.json`
- CMake 模块: `cmake/Dependencies.cmake`（依赖查找、MSVC 默认选项、输出目录统一）、`cmake/GrpcCodegen.cmake`（proto 代码生成）
- 子项目入口:
  - `server/CMakeLists.txt` → 聚合 common/GateServer/StatusServer/ChatServer/ResourceServer
  - `client/llfcchat/CMakeLists.txt` → Qt 客户端可执行文件
  - `tests/CMakeLists.txt` → 单元测试与可选集成测试
- Proto 定义: `proto/chat_service/chat.proto`、`proto/status_service/status.proto`、`proto/verify_service/verify.proto`

## 3. 架构与约定
- **分层 CMake 结构**：每个子服务/客户端都有独立 `CMakeLists.txt`，通过 `add_subdirectory` 组合，支持从仓库根或子目录单独配置。
- **统一的依赖解析**：`llfc_find_server_dependencies()` / `llfc_find_client_dependencies()` 集中 find_package，避免各子项目重复声明。
- **统一的 MSVC 默认项**：`llfc_apply_msvc_defaults()` 设置 `/utf-8 /wd4819` 及静态运行时 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`。
- **统一的产物输出**：`llfc_set_runtime_outdir()` 将所有可执行文件输出到 `out/run/<CONFIG>/<subdir>/`，按配置分目录组织。
- **Proto 作为静态库**：每个 `.proto` 通过 `llfc_add_proto_library` 生成 `.pb.cc/.h` 和 `.grpc.pb.cc/.h`，打包成 STATIC 库供 gRPC 客户端/服务端链接。
- **Qt 客户端构建**：启用 AUTOMOC/AUTOUIC/AUTORCC，显式声明 `.ui` 文件和资源 `rc.qrc`，并通过 `qt5_import_plugins` 静态嵌入平台、图像格式、样式插件。
- **测试分层**：`worker_pool_tests` 和 `password_hash_tests` 始终注册；`im_integration_tests` 仅在 `LLFC_RUN_INTEGRATION_TESTS=ON` 时启用，且每个 scenario 单独 `add_test` 并标记 `LABELS integration`，串行运行。

## 4. 约定与约束
- **必须使用 Ninja Multi-Config 生成器**：根与子 CMakeLists.txt 均通过 `message(FATAL_ERROR ...)` 强制校验。
- **禁止 in-source 构建**：所有入口检查 `CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR` 并报错。
- **vcpkg 工具链必须通过 `VCPKG_ROOT` 环境变量注入**：`CMakePresets.json` 中 `toolchainFile` 指向 `$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`，triplet 固定为 `x64-windows-static-md`。
- **C++ 标准**：服务端与测试使用 C++17，Qt 客户端因历史原因仍为 C++11（在 client CMakeLists.txt 中显式覆盖）。
- **集成测试需外部依赖**：Redis、MySQL 以及已启动的服务进程，默认关闭以避免 CI 失败。
- **Proto 路径固定**：`llfc_ensure_*_proto()` 函数硬编码 `${LLFC_REPO_ROOT}/proto/<service>/<name>.proto` 路径。
- **Windows 兼容宏**：集成测试强制 `_WIN32_WINNT=0x0601`、`WIN32_LEAN_AND_MEAN`、`NOMINMAX` 以适配 Boost.Asio 与 hiredis。