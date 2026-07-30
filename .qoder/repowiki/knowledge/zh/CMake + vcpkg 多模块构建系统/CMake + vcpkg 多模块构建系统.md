---
kind: build_system
name: CMake + vcpkg 多模块构建系统
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
---

## 构建系统与工具链

项目采用 **CMake 3.25+** 作为统一构建系统，配合 **vcpkg** 进行依赖管理，强制使用 **Ninja Multi-Config** 生成器，支持 Debug/Release/RelWithDebInfo/MinSizeRel 多配置并行构建。所有 C++ 目标默认启用 C++17（客户端 Qt 部分为 C++11），禁止编译器扩展。

## 核心文件与职责

- `CMakeLists.txt`：根入口，设置 vcpkg 路径、包含自定义 cmake 模块、按顺序添加 server/ 和 client/llfcchat/ 子目录
- `CMakePresets.json`：预定义 Windows Ninja 多配置 preset，指定 `x64-windows-static-md` 三元组及 `vcpkg_installed` 输出目录
- `vcpkg.json`：声明所有第三方依赖（Boost、gRPC、protobuf、hiredis、mysql-connector-cpp、Qt5-base/imageformats）
- `cmake/Dependencies.cmake`：封装依赖查找函数 `llfc_find_server_dependencies()` / `llfc_find_client_dependencies()`，提供 MSVC 默认编译选项、仓库根路径计算、运行时输出目录统一放置到 `out/run/<CONFIG>/<subdir>`
- `cmake/GrpcCodegen.cmake`：封装 gRPC/protobuf 代码生成，自动从 `proto/` 目录生成 `.pb.cc/.h` 和 `.grpc.pb.cc/.h`，并创建静态库目标 `llfc_chat_proto` / `llfc_status_proto` / `llfc_verify_proto`

## 架构与约定

### 目录结构
- `server/`：四个独立服务端可执行文件（GateServer、StatusServer、ChatServer、ResourceServer），每个服务有独立的 `include/`、`src/`、`config/`、`CMakeLists.txt`
- `client/llfcchat/`：Qt GUI 客户端，使用 AUTOMOC/AUTOUIC/AUTORCC 自动处理 .ui/.qrc/.moc
- `proto/`：gRPC 接口定义，按服务分目录（chat_service、status_service、verify_service）
- `out/`：构建产物输出目录，按配置和子模块组织二进制文件

### 构建约定
- 禁止 in-source 构建，必须在独立 build 目录中执行
- 所有可执行文件通过 `llfc_set_runtime_outdir()` 统一输出到 `out/run/<CONFIG>/<subdir>`
- 服务端默认链接 Boost.Asio/Beast、nlohmann-json、hiredis、mysql-connector-cpp（JDBC 静态库）、gRPC/protobuf
- 客户端链接 Qt5 Core/Gui/Network/Widgets，并通过 `qt5_import_plugins()` 静态嵌入平台、图像格式、样式插件
- 每个服务的配置文件在 POST_BUILD 阶段自动复制到可执行文件同级目录

### 依赖管理
- vcpkg 使用固定 baseline `a51bb4d1434e6d0927ff79db803bbed85222b5df` 确保可重复构建
- 所有依赖通过 `find_package(... CONFIG REQUIRED)` 查找，由 vcpkg toolchain 提供
- 客户端 Qt 仅启用必要组件（Core/Gui/Network/Widgets），减少体积

## 开发者规范

1. **必须使用 Ninja Multi-Config 生成器**，可通过 `CMakePresets.json` 中的 `windows-ninja` preset 快速配置
2. **禁止 in-source 构建**，必须在独立目录执行 cmake 配置
3. **新增服务时**：复制现有服务目录结构，在 `server/CMakeLists.txt` 中添加 `add_subdirectory()`，并在 `Dependencies.cmake` 中按需调整依赖
4. **新增 proto 文件时**：放在 `proto/<service>/` 目录下，调用 `llfc_add_proto_library()` 或对应的 `llfc_ensure_*_proto()` 宏
5. **新增依赖时**：在 `vcpkg.json` 中声明，确保 vcpkg 已安装对应 triplet
6. **资源部署**：通过 `add_custom_command(TARGET ... POST_BUILD ...)` 将配置文件、静态资源复制到输出目录
7. **跨平台注意**：当前 preset 仅配置 Windows，如需 Linux/macOS 需补充相应 preset 和 triplet