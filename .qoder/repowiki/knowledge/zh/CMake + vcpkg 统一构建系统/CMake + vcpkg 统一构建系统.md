---
kind: build_system
name: CMake + vcpkg 统一构建系统
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
    - server/ChatServer/CMakeLists.txt
    - server/GateServer/CMakeLists.txt
    - server/ResourceServer/CMakeLists.txt
    - server/StatusServer/CMakeLists.txt
---

## 1. 构建系统与工具链
- **构建系统**: CMake 3.25+，强制使用 `Ninja Multi-Config` 生成器，禁止 in-source 构建。
- **依赖管理**: vcpkg 清单模式（`vcpkg.json`），固定 baseline `a51bb4d1434e6d0927ff79db8033bed8522b85df`，安装目录锁定在 `vcpkg_installed/`。
- **预设配置**: `CMakePresets.json` 提供 `windows-ninja` 配置预设，默认 triplet 为 `x64-windows-static-md`，支持 Debug/Release 双配置。
- **语言标准**: 服务端与客户端均要求 C++17（客户端 CMake 中显式设为 11，但通过 `target_compile_features` 提升）。

## 2. 核心构建文件与模块
- **根 CMakeLists.txt**: 入口聚合 `server/` 与 `client/llfcchat/` 两个子项目，设置 `LLFC_REPO_ROOT`、`VCPKG_MANIFEST_DIR`、`VCPKG_INSTALLED_DIR`。
- **cmake/Dependencies.cmake**: 封装 `llfc_find_server_dependencies()` / `llfc_find_client_dependencies()`，统一 find_package；提供 `llfc_apply_msvc_defaults()` 与 `llfc_set_runtime_outdir()` 宏，将产物输出到 `out/run/<CONFIG>/<subdir>/`。
- **cmake/GrpcCodegen.cmake**: 封装 `llfc_add_proto_library()`，自动调用 `protoc` + `grpc_cpp_plugin` 生成 `.pb.cc/.h` 与 `.grpc.pb.cc/.h`，并创建静态库目标；提供 `llfc_ensure_chat_proto()` / `llfc_ensure_control_proto()` / `llfc_ensure_resource_proto()` 快捷函数。
- **各服务 CMakeLists.txt**: `ChatServer`、`GateServer`、`ResourceServer`、`StatusServer` 结构一致：声明源文件、链接对应 proto 库与 Boost/JsonCpp/hiredis/mysql-concpp，POST_BUILD 复制配置文件。
- **客户端 CMakeLists.txt**: Qt5 应用，启用 AUTOMOC/AUTOUIC/AUTORCC，通过 `qt5_import_plugins` 静态引入 QWindowsIntegrationPlugin、QGifPlugin、QJpegPlugin、QWebpPlugin、QWindowsVistaStylePlugin，POST_BUILD 复制 `config.ini` 与 `assets/static/`。

## 3. 架构与约定
- **分层 CMake 结构**: 根 → `server/`（聚合四个微服务）→ 每个服务独立 `CMakeLists.txt`；客户端独立位于 `client/llfcchat/`。
- **Proto 代码生成**: 所有 gRPC/Protobuf 定义集中在 `server/proto/{chat,control,resource}/message.proto`，通过 `GrpcCodegen.cmake` 在构建时生成源码至 `build/generated/<target>/`，不污染源码树。
- **统一输出布局**: 所有可执行文件输出到 `out/run/<CONFIG>/<service_name>/`，便于按服务区分部署。
- **多实例支持**: ChatServer 的 POST_BUILD 会同时复制一份到 `chatserver2/` 目录，配合不同 `config.ini` 实现双实例。
- **MSVC 特定优化**: 统一启用 `/utf-8`、禁用警告 4819，运行时库设置为多线程 DLL（Debug 下为 DebugDLL）。

## 4. 开发者应遵循的规则
- **必须使用 Ninja Multi-Config 生成器**，且必须在独立 build 目录中配置（in-source 构建会被拒绝）。
- **新增依赖**: 在 `vcpkg.json` 中添加包名，确保已安装对应 triplet；服务端依赖通过 `llfc_find_server_dependencies()` 统一查找。
- **新增 Proto 接口**: 在 `server/proto/<domain>/message.proto` 添加定义后，在 `GrpcCodegen.cmake` 中注册对应的 `llfc_ensure_*_proto()` 函数，并在目标 CMakeLists 中调用。
- **新增服务**: 复制现有服务的 `CMakeLists.txt` 模板，修改目标名、源文件列表、链接库与 POST_BUILD 配置复制命令。
- **Qt 客户端新增 UI**: 在 `ui/` 目录下添加 `.ui` 文件，并在 `LLFCCHAT_UIS` 列表中注册，AUTOUIC 会自动处理。
- **资源文件**: 静态资源放入 `resources/`，通过 `rc.qrc` 注册，AUTORCC 自动生成资源头。
- **配置部署**: 每个服务的 `config.ini` 通过 POST_BUILD 复制到输出目录，保持配置与二进制分离。