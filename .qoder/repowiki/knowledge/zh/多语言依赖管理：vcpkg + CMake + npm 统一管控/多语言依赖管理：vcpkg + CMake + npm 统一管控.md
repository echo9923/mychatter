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
    - CMakePresets.json
    - server/VarifyServer/package.json
    - server/VarifyServer/package-lock.json
    - webrtc-demo/server/package.json
    - webrtc-demo/server/package-lock.json
---

本项目采用**多工具链混合依赖管理**策略：C++ 部分使用 vcpkg 作为包管理器，通过 CMake 集成；Node.js 子服务使用 npm（package.json + package-lock.json）管理依赖。两种体系在仓库中并存，但各自保持独立、可复现的构建流程。

### 1. C++ 依赖：vcpkg + CMake
- **声明文件**：根目录 `vcpkg.json` 集中声明所有 C++ 第三方库，包括 Boost（asio/beast/date-time/filesystem/property-tree/uuid）、nlohmann-json、gRPC、protobuf、hiredis、mysql-connector-cpp（带 jdbc 特性）、Qt5（base 与 imageformats/webp）。
- **版本锁定**：通过 `builtin-baseline` 指向 vcpkg 官方基线快照 `a51bb4d1434e6d0927ff79db8522b85df`，确保跨环境一致。
- **安装位置**：`VCPKG_INSTALLED_DIR` 固定为 `./vcpkg_installed`，由根 `CMakeLists.txt` 和 `cmake/Dependencies.cmake` 中的 `llfc_setup_standalone_vcpkg` 宏强制设置。
- **CMake 集成**：
  - 根 `CMakeLists.txt` 要求必须使用 `Ninja Multi-Config` 生成器，禁止 in-source 构建。
  - `cmake/Dependencies.cmake` 提供 `llfc_find_server_dependencies()` 与 `llfc_find_client_dependencies()` 两个函数，分别用 `find_package(... CONFIG REQUIRED)` 查找各依赖。
  - `CMakePresets.json` 预配置 Windows + MSVC + Ninja 的 toolchain 路径、目标 triplet `x64-windows-static-md` 及缓存变量。
- **产物输出**：通过 `llfc_set_runtime_outdir` 将所有可执行文件统一输出到 `out/run/<CONFIG>/<subdir>`。

### 2. Node.js 依赖：npm
- **验证服务**：`server/VarifyServer/package.json` 声明 `@grpc/grpc-js`、`ioredis`、`nodemailer`、`uuid` 等依赖，并附带 `package-lock.json`（lockfileVersion 1）锁定精确版本。
- **WebRTC Demo**：`webrtc-demo/server/package.json` 声明 `express` 与 `ws`，同样有 `package-lock.json`（lockfileVersion 3）。
- 两个子模块各自独立维护 lock 文件，互不影响。

### 3. 架构与约定
- **分层清晰**：C++ 依赖集中在根 `vcpkg.json`，Node 依赖分散在各子目录 `package.json`，符合“按语言隔离”的原则。
- **可重复构建**：vcpkg 通过 baseline 锁定上游版本，npm 通过 lock 文件锁定传递依赖树，两者均支持离线/CI 场景。
- **构建入口统一**：开发者只需运行 `cmake --preset windows-ninja && cmake --build --preset windows-debug`，即可自动拉取 vcpkg 依赖并编译全部目标。

### 4. 开发者应遵循的规则
- 新增 C++ 依赖时，编辑根 `vcpkg.json` 并在对应服务的 `CMakeLists.txt` 中调用 `llfc_find_server_dependencies()` 或 `llfc_find_client_dependencies()`。
- 新增 Node.js 依赖时，仅在相应子目录的 `package.json` 中添加，并确保提交对应的 `package-lock.json`。
- 禁止手动修改 `vcpkg_installed` 目录内容，所有安装应由 vcpkg 自动完成。
- 切换分支后首次构建前需执行 `vcpkg install` 以同步依赖树。