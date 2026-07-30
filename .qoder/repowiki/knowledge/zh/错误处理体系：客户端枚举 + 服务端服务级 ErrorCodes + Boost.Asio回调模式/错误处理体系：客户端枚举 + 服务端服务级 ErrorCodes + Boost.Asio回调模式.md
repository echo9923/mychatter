---
kind: error_handling
name: 错误处理体系：客户端枚举 + 服务端服务级 ErrorCodes + Boost.Asio回调模式
category: error_handling
scope:
    - '**'
source_files:
    - client/llfcchat/include/global.h
    - client/llfcchat/include/httpmgr.h
    - client/llfcchat/include/logindialog.h
    - client/llfcchat/include/registerdialog.h
    - client/llfcchat/include/resetdialog.h
    - client/llfcchat/src/filetcpmgr.cpp
    - server/ChatServer/include/const.h
    - server/GateServer/include/const.h
    - server/ResourceServer/include/const.h
    - server/StatusServer/include/const.h
    - server/ChatServer/include/CSession.h
    - server/ChatServer/include/MysqlDao.h
---

## 1. 使用的系统与模式
- **客户端（Qt/C++）**：使用 `enum ErrorCodes` 作为统一的错误码，通过信号槽机制将 `ErrorCodes` 与请求结果一起回传给 UI 层；网络 I/O 错误由 Qt 的 `QTcpSocket::error` / `QNetworkReply` 等信号提供。
- **服务端（C++/Boost.Asio）**：每个服务模块在各自的 `include/const.h` 中定义独立的 `enum ErrorCodes`，并通过 `boost::system::error_code` 作为异步回调参数进行错误传播；数据库访问使用 MySQL Connector/C++ 的 `sql::SQLException` 异常。
- **Node.js 验证服务**：通过 JSON 响应中的 `error` 字段传递错误码，供 C++ GateServer 消费。

## 2. 关键文件与位置
- 客户端错误码与通用类型：`client/llfcchat/include/global.h`（`enum ErrorCodes`、`ReqId`、`Modules`、`TipErr` 等）
- HTTP 管理器（客户端）：`client/llfcchat/include/httpmgr.h`（`sig_http_finish` 等信号携带 `ErrorCodes`）
- 登录/注册/重置对话框：`client/llfcchat/include/logindialog.h`、`registerdialog.h`、`resetdialog.h`（槽函数接收 `ErrorCodes err`）
- 服务端各模块错误码：
  - `server/ChatServer/include/const.h`
  - `server/GateServer/include/const.h`
  - `server/ResourceServer/include/const.h`
  - `server/StatusServer/include/const.h`
- Asio 会话与错误回调：`server/ChatServer/include/CSession.h`、`server/ResourceServer/include/CSession.h`（`HandleWrite`、`asyncReadFull` 等使用 `boost::system::error_code`）
- MySQL 连接池与异常：`server/ChatServer/include/MysqlDao.h`（`sql::SQLException` 捕获）
- 文件传输错误处理示例：`client/llfcchat/src/filetcpmgr.cpp`（解析 JSON 中的 `error` 字段并分支处理）

## 3. 架构与约定
- **分层错误码**：客户端与服务端各自维护 `ErrorCodes` 枚举，值域互不冲突（客户端 0/1/2，服务端从 1001 起），便于跨进程识别来源。
- **统一返回结构**：服务端所有 RPC/HTTP/TCP 响应均包含 `error` 字段，客户端统一解析该字段并与 `SUCCESS` 比较，决定 UI 提示或继续流程。
- **异步 I/O 错误传播**：Boost.Asio 采用 `error_code` 回调风格，所有读写操作通过 lambda 捕获 `error_code` 并在其中判断失败路径，避免抛出异常。
- **数据库异常本地化**：MySQL 操作通过 `try/catch (sql::SQLException&)` 捕获，记录日志后降级为业务错误码返回上层。
- **UI 层错误展示**：客户端通过 `qDebug()` / `qWarning()` 输出调试信息，并通过 `QMessageBox::critical` 向用户提示严重错误（如图片加载失败）。

## 4. 开发者应遵循的规则
1. **新增错误码时**：在服务端对应服务的 `include/const.h` 中追加枚举项，并确保客户端 `global.h` 中的 `ErrorCodes` 与之对齐（或通过协议字段映射）。
2. **网络 I/O 错误**：必须检查 `boost::system::error_code` 和 Qt Socket 信号返回值，不得忽略失败分支。
3. **JSON 解析失败**：当 `recvObj.contains("error")` 为假时，默认回退到 `ERR_JSON`，保持健壮性。
4. **数据库异常**：所有 `sql::Connection` / `Statement` 调用需包裹 `try/catch`，捕获后记录日志并返回业务错误码。
5. **UI 反馈**：对可恢复错误使用 `qDebug()/qWarning()`，对用户可见错误使用 `QMessageBox` 或状态标签更新。
6. **禁止抛异常跨越边界**：C++ 服务端核心逻辑不使用 `throw` 跨模块传播错误，统一通过 `error_code` 和业务错误码返回。