---
kind: dependency_management
name: 基于 vcpkg + CMake 的 C++ 依赖与 Node.js 子模块包管理
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
---

## 1. 使用的系统/工具
- **C++ 依赖**：使用 **vcpkg**（manifest 模式）作为包管理器，配合 **CMake** 通过 `find_package` 解析。根目录 `vcpkg.json` 声明所有第三方库，构建产物安装到仓库内的 `vcpkg_installed/` 目录。
- **gRPC/Protobuf**：通过 vcpkg 提供的 `grpc`、`protobuf` 目标，在 `cmake/GrpcCodegen.cmake` 中用自定义函数 `llfc_add_proto_library()` 调用 `protoc` 和 `grpc_cpp_plugin` 将 `proto/*.proto` 生成 `.pb.cc/.h` 与 gRPC stub 并编译为静态库。
- **Qt 依赖**：通过 vcpkg 的 `qt5-base`、`qt5-imageformats` 提供，客户端通过 `find_package(Qt5 CONFIG REQUIRED COMPONENTS Core Gui Network Widgets Sql)` 引入。
- **Node.js 服务**：`server/VarifyServer/` 是独立的 Node.js 服务，使用 `package.json` + `package-lock.json` 管理依赖；`webrtc-demo/server/` 也有独立的 `package.json`。

## 2. 关键文件
- `vcpkg.json`：集中声明 C++ 第三方依赖（Boost.Asio/Beast/date-time/filesystem/property-tree/uuid、nlohmann-json、grpc、protobuf、hiredis、openssl、mysql-connector-cpp[jdbc]、qt5-base[sqlite3plugin]、qt5-imageformats[webp]），并通过 `builtin-baseline` 锁定 vcpkg 版本基线。
- `CMakeLists.txt`（根）：强制要求 Ninja Multi-Config 生成器、禁止 in-source 构建，设置 `VCPKG_MANIFEST_DIR` 与 `VCPKG_INSTALLED_DIR` 指向仓库内路径，随后 include `Dependencies.cmake` 与 `GrpcCodegen.cmake`。
- `cmake/Dependencies.cmake`：封装 `llfc_find_server_dependencies()` 与 `llfc_find_client_dependencies()`，统一 `find_package` 调用；提供 `llfc_apply_msvc_defaults()` 固定 MSVC 运行时为 `/MD`（MultiThreaded$<$<CONFIG:Debug>:Debug>DLL）；定义 `llfc_set_runtime_outdir()` 将所有可执行输出到 `out/run/<CONFIG>/<subdir>`。
- `cmake/GrpcCodegen.cmake`：封装 proto 代码生成逻辑，按 target 名在 `<build>/generated/<target>` 下产出，并暴露 `llfc_ensure_chat_proto` / `llfc_ensure_verify_proto` / `llfc_ensure_status_proto` 供各服务按需引入。
- `server/VarifyServer/package.json` + `package-lock.json`：声明 `@grpc/grpc-js`、`@grpc/proto-loader`、`ioredis`、`nodemailer`、`uuid` 等依赖。
- `webrtc-demo/server/package.json`：独立示例服务的依赖声明（express、ws）。

## 3. 架构与约定
- **单一 manifest 源**：所有 C++ 依赖集中在根 `vcpkg.json`，各子工程（`server/*`、`client/llfcchat`、`tests`）不重复声明，仅通过 `find_package` 引用。
- **共享 CMake 抽象层**：新增依赖时只需修改 `cmake/Dependencies.cmake` 中的对应函数，避免在各子 `CMakeLists.txt` 散落 `find_package` 调用。
- **Proto 生成隔离**：每个 proto 服务通过 `llfc_add_proto_library(<name> <file>)` 生成独立静态库目标，避免全局污染；生成的头文件路径由 `target_include_directories` 自动注入。
- **Node.js 子模块隔离**：只有 `server/VarifyServer/` 与 `webrtc-demo/server/` 各自维护独立 `package.json`，与 C++ 主工程完全解耦。
- **构建产物布局**：通过 `llfc_set_runtime_outdir()` 统一把二进制输出到 `out/run/<CONFIG>/<service>`，便于区分不同服务。

## 4. 约定与约束
- **必须使用 Ninja Multi-Config 生成器**：根 `CMakeLists.txt` 在检测到非该生成器时直接 `message(FATAL_ERROR ...)` 终止配置。
- **禁止 in-source 构建**：当 `CMAKE_SOURCE_DIR == CMAKE_BINARY_DIR` 时报错退出。
- **vcpkg 安装目录固定**：通过 `VCPKG_INSTALLED_DIR` 强制指向仓库根下的 `vcpkg_installed/`，保证依赖随仓库一起被检出。
- **MSVC 运行时锁定**：`llfc_apply_msvc_defaults()` 对所有目标强制使用 `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`，避免 Debug/Release 混用 CRT 的问题。
- **Proto 文件位置约束**：`GrpcCodegen.cmake` 中三个 ensure_* 函数硬编码了 `proto/chat_service/chat.proto`、`proto/status_service/status.proto`、`proto/verify_service/verify.proto`，新增 proto 需同步扩展这些函数。
- **Node.js 依赖锁定**：`server/VarifyServer/package-lock.json` 存在，表明应通过 `npm ci` 或 `npm install` 保持 lockfile 一致；`webrtc-demo/server/` 同样有 `package-lock.json`。
- **集成测试外部依赖开关**：根 CMake 提供 `LLFC_RUN_INTEGRATION_TESTS` 选项，默认 OFF，因为需要真实 Redis/MySQL 实例，避免 CI 因缺少外部服务而失败。