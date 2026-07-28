# 编译指南

## 环境

| 组件 | 路径 |
|------|------|
| Visual Studio 2026 BuildTools | `C:\Program Files\Microsoft Visual Studio\18\BuildTools` |
| MSVC 工具集 | `VC\Tools\MSVC\14.51.36231` |
| CMake（VS 内置） | `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| Ninja（VS 内置） | `<VS>\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe` |
| vcpkg | `C:\tools\vcpkg-a51bb4d\vcpkg.exe` |
| vcpkg 已安装包 | 项目根目录 `vcpkg_installed\x64-windows-static-md\` |

## 编译步骤

必须在 **PowerShell** 中执行（需要先加载 MSVC 环境）：

```powershell
# 1. 加载 VS 开发环境
& 'C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -SkipAutomaticLocation

# 2. 进入项目
cd D:\codeproject\cpp\llfcchat

# 3. 配置（首次或 CMakeLists 变动后）
cmake --preset windows-ninja

# 4. 编译全部服务器（Debug）
cmake --build out/build/windows-ninja --config Debug --target GateServer StatusServer ChatServer ResourceServer

# 5. 编译客户端
cmake --build out/build/windows-ninja --config Debug --target llfcchat
```

Release 构建将 `Debug` 换为 `Release`。

## 输出位置

```
out/run/<Config>/GateServer/GateServer.exe
out/run/<Config>/StatusServer/StatusServer.exe
out/run/<Config>/chatserver1/ChatServer.exe
out/run/<Config>/ResourceServer/ResourceServer.exe
```

## vcpkg 说明

- 清单模式，`vcpkg.json` 在项目根目录
- triplet: `x64-windows-static-md`
- `VCPKG_INSTALLED_DIR` 指向项目内 `vcpkg_installed/`（已随仓库提交）
- CMakePresets.json 中 toolchainFile 引用 `$env{VCPKG_ROOT}`，若未设置该环境变量，CMake 仍可使用已有的 `vcpkg_installed/` 完成配置

## 注意事项

- 生成器固定为 `Ninja Multi-Config`，CMakeLists 中有强制检查
- 服务器使用 C++17，Qt 客户端使用 C++11
- JSON 库为 nlohmann-json，通过 vcpkg 管理
