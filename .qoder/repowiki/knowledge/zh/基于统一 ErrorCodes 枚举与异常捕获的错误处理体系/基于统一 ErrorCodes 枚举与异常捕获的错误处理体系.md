---
kind: error_handling
name: 基于统一 ErrorCodes 枚举与异常捕获的错误处理体系
category: error_handling
scope:
    - '**'
source_files:
    - server/ChatServer/include/const.h
    - server/GateServer/include/const.h
    - server/ResourceServer/include/const.h
    - server/StatusServer/include/const.h
    - client/llfcchat/include/global.h
    - client/llfcchat/src/httpmgr.cpp
    - server/ChatServer/src/CSession.cpp
    - server/ChatServer/src/MysqlDao.cpp
    - server/GateServer/include/VerifyGrpcClient.h
    - tests/integration/im_common.h
---

## 1. 总体方案

本仓库采用 **每服务独立 `enum ErrorCodes` + 客户端镜像 + 集成测试常量** 的分布式错误码体系，配合 C++ 标准异常（`std::exception`、`sql::SQLException`）和 gRPC `Status` 进行错误传播；上层通过 JSON 字段 `error` 将错误码回传给 Qt 客户端，由客户端根据 `ErrorCodes` 分支决定重试/失败 UI。

- 服务端各进程（ChatServer、GateServer、ResourceServer、StatusServer）均在自身 `include/const.h` 中定义同名 `enum ErrorCodes`，数值段约定为：
  - `0` = Success
  - `1001~1011` = 通用业务错误（JSON 解析、RPC 失败、验证码过期/错误、用户存在、密码错误、邮箱不匹配、密码更新失败、Token 失效、UID 无效）
  - `1012~1027` = 各服务专属错误（如 ChatServer 的 `MESSAGE_STORE_FAILED=1014`、`RECIPIENT_OFFLINE=1015`、`SERVER_BUSY=1016`、`MESSAGE_CONFLICT=1017`、`NoAvailableChatServer=1018`、`ResourceInvalid=1019`、`ResourceSizeExceeded=1020`；ResourceServer 的 `FileOffsetInvalid=1018`、`MsgIdErr=1022`、`FileHashMismatch=1023`、`ResourceNotReady=1025`、`ResourceForbidden=1026`、`ResourceStateInvalid=1027` 等）
- 客户端 `client/llfcchat/include/global.h` 中的 `enum ErrorCodes` 是服务端错误码的 **镜像子集**（含 `ERR_JSON=1`、`ERR_NETWORK=2` 等客户端本地错误），用于 HTTP/TCP 回调统一返回。
- 集成测试 `tests/integration/im_common.h` 以 `inline constexpr int ERR_*` 形式再次镜像服务端错误码，保证测试断言与服务端行为一致。

## 2. 关键文件与位置

| 层级 | 文件 | 作用 |
|---|---|---|
| 服务端公共协议 | `server/ChatServer/include/const.h` | 定义 TCP 消息类型、`ReqToRspId` 映射表、核心 `ErrorCodes` |
| Gate/Status 服务 | `server/GateServer/include/const.h`、`server/StatusServer/include/const.h` | 复用相同 1001~1018 错误码段 |
| 资源服务 | `server/ResourceServer/include/const.h` | 扩展 1012~1027 资源链路错误码 |
| 客户端全局 | `client/llfcchat/include/global.h` | 客户端 `ErrorCodes`、`Modules`、`TransferState` 等 |
| HTTP 错误入口 | `client/llfcchat/src/httpmgr.cpp` | QNetworkReply 错误 → `sig_http_finish(..., ErrorCodes::ERR_NETWORK)` |
| TCP 会话层 | `server/ChatServer/src/CSession.cpp` | 网络 I/O 异常捕获、队列拒绝时回送 `SERVER_BUSY` |
| DAO 层 | `server/ChatServer/src/MysqlDao.cpp` | `sql::SQLException` 捕获并记录 MySQL error code/SQLState |
| gRPC 客户端 | `server/GateServer/include/VerifyGrpcClient.h` | gRPC `Status` 非 ok 时设置 `reply.error(RPCFailed)` |
| 集成测试 | `tests/integration/im_common.h` | 错误码常量镜像，供场景断言使用 |

## 3. 架构与约定

### 3.1 错误码作为跨进程契约
所有跨服务通信（TCP JSON、HTTP JSON、gRPC protobuf）均以 `error` 字段携带 `ErrorCodes` 整型值。调用方只依赖数值，不依赖字符串描述，因此每个服务都维护一份 `enum ErrorCodes` 以保持编译期可读性。

### 3.2 异常捕获策略
- **网络 I/O 层**（`CSession.cpp`）：在异步 read/write 回调中用 `try/catch (std::exception&)` 包裹，捕获后打印 `e.what()` 并关闭连接或清理 session，不向上抛出。
- **数据库层**（`MysqlDao.cpp`）：对 `sql::SQLException` 单独 catch，记录 `what()`、MySQL error code、SQLState 后返回 `false`/默认值，由上层逻辑判断业务含义。
- **gRPC 调用**（`VerifyGrpcClient.h`）：检查 `status.ok()`，失败时填充 `reply.set_error(ErrorCodes::RPCFailed)`，不抛异常。
- **业务逻辑层**（`LogicSystem.cpp`）：多处 `catch (...)` 吞掉未知异常，确保单条消息处理失败不影响 worker 继续处理后续消息。

### 3.3 不可恢复错误的快速失败
当 TCP 接收队列满（`PostMsgToQue` 返回 false）时，`CSession` 通过 `ReqToRspId` 查找对应响应 ID，原子发送 `{"error": SERVER_BUSY}` 终帧后立即关闭连接，避免消息入队与持久化。

### 3.4 客户端错误分类
客户端 `httpmgr.cpp` 将 QNetworkReply 的网络错误统一映射为 `ErrorCodes::ERR_NETWORK`，成功则传 `SUCCESS`；业务错误码由后端 JSON 中的 `error` 字段下发，UI 层按 `ErrorCodes` 分支显示提示或触发重传。

## 4. 约定与约束

- **错误码数值段必须保持向后兼容**：新增错误码只能追加到各自服务的 `enum ErrorCodes` 末尾，不得修改已有编号（否则客户端/测试镜像会误判语义）。
- **客户端仅镜像服务端错误码的子集**：客户端 `global.h` 的 `ErrorCodes` 明确注释“服务端投递错误码（镜像 ChatServer const.h）”，新增服务端错误需同步更新客户端镜像。
- **测试侧必须镜像服务端错误码**：`im_common.h` 以 `inline constexpr int ERR_*` 形式复制服务端错误码，用于集成测试断言，新增错误码需在此补充。
- **DAO 层不向上传播 SQL 异常**：所有 `sql::SQLException` 被捕获并转为布尔返回值，上层通过业务错误码表达失败原因。
- **gRPC 失败不抛异常**：所有 gRPC 调用均显式检查 `Status::ok()`，失败时填充 `error` 字段返回给调用方，由调用方决定是否重试。
- **无统一的异常基类或 Result<T,E>**：当前代码未引入自定义异常类型或 Rust 风格 Result，错误主要通过错误码 + 返回值 + 日志输出传递。
- **Defer RAII 用于资源释放**：`common/include/Defer.h` 被多个 DAO 方法用于确保 MySQL 连接归还，即使 throw 也能正确释放资源。

## 5. 缺失与改进点

- 没有统一的错误类型封装（如 `Result<Resp, ErrorCode>`），错误信息散落在 JSON、gRPC field 和日志中。
- 客户端与服务端错误码完全靠手工镜像，缺乏生成器或校验脚本防止漂移。
- 日志输出使用 `std::cout`/`std::cerr`，未接入结构化日志框架，不利于生产排查。
- 未发现 `panic/recover` 模式（C++ 无此概念），但 `catch (...)` 广泛使用，可能掩盖真正 bug。