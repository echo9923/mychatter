---
kind: error_handling
name: 错误处理体系：统一错误码 + RAII + 异常捕获 + gRPC/HTTP 分层处理
category: error_handling
scope:
    - '**'
source_files:
    - server/ChatServer/include/const.h
    - client/llfcchat/include/global.h
    - server/common/include/Defer.h
    - server/ChatServer/src/CSession.cpp
    - server/ChatServer/src/MysqlDao.cpp
    - server/ChatServer/src/LogicSystem.cpp
    - server/ChatServer/src/ChatGrpcClient.cpp
    - server/GateServer/include/const.h
---

本仓库在 C++ 服务端与 Qt 客户端之间采用分层、统一的错误处理策略，核心由「统一应用层错误码枚举」+「RAII 资源清理」+「局部 try/catch 捕获」+「gRPC/HTTP 传输层状态」构成。

1. 统一应用层错误码（ErrorCodes）
- ChatServer/GateServer/ResourceServer/StatusServer 各自在 include/const.h 中定义 ErrorCodes 枚举，语义覆盖 JSON 解析失败、RPC 失败、验证码过期/错误、用户存在、密码错误、Token 失效、消息持久化失败、接收方离线、服务器繁忙、消息冲突等。客户端 global.h 镜像了与服务端一致的子集（如 MESSAGE_STORE_FAILED=1014、RECIPIENT_OFFLINE=1015、SERVER_BUSY=1016、MESSAGE_CONFLICT=1017），用于 UI 提示与重传策略判断。
- 错误码通过 JSON 字段 `error` 在 TCP/gRPC/HTTP 响应中回传，调用方按值分支处理（成功/可重试/不可重试）。

2. 传输层错误与 gRPC 状态码
- gRPC 客户端（ChatGrpcClient、ResourceServer/ChatServerGrpcClient）使用 grpc::StatusCode 区分 UNAVAILABLE/DEADLINE_EXCEEDED/RESOURCE_EXHAUSTED 等可重试错误，并结合配置 RpcDeadlineMs/RpcMaxAttempts/RpcBackoffMs 实现指数退避重试；仅对 SERVER_BUSY(1016) 做业务级重试，其他应用错误立即停止。
- HTTP(Beast) 请求失败时直接返回对应 ErrorCodes（如 TokenInvalid、VarifyExpired），由 GateServer 统一封装。

3. 异常捕获与 RAII 清理
- 网络 IO（Boost.Asio）回调中使用 try/catch(std::exception) 包裹解析与路由逻辑，捕获后打印日志并安全关闭连接（CSession.cpp 多处）。SQL 操作捕获 sql::SQLException，记录 error code 与 SQLState 后返回 false/-1。
- 自定义 Defer 类（common/include/Defer.h）广泛用于 RAII 式资源释放：MySQL 连接池归还、JSON 响应构造后发送、gRPC 响应默认填充等，确保异常路径也能正确清理。

4. 客户端错误呈现
- Qt 客户端未使用 QMessageBox/qDebug 等弹窗或调试输出作为错误处理主路径，而是通过全局 ReqId/ErrorCodes 映射到界面提示（TipErr、MsgStatus、TransferState），结合 TCPMgr/HttpMgr 的错误回调更新 UI 状态（如 SEND_FAILED、UN_UPLOAD）。

5. 设计约定与约束
- 所有跨服务通信必须携带 error 字段，禁止静默失败；无法确定响应 ID 的通知类消息（ReqToRspId 返回 0）直接关闭连接而非盲发。
- 可重试错误严格限定为 gRPC transport 级别（UNAVAILABLE/DEADLINE_EXCEEDED/RESOURCE_EXHAUSTED）与业务 SERVER_BUSY，其余错误视为终态。
- 配置读取失败一律回退默认值（ReadWorkerCount/ReadDeliveryInt 的 catch(...) 分支），避免启动期崩溃。
- 登录认证失败后立即关闭连接，防止后续消息被误处理（CSession.cpp 登录前校验 uid/routing_uid）。

关键文件：
- server/ChatServer/include/const.h — 服务端统一 ErrorCodes、MSG_IDS、ReqToRspId 映射
- client/llfcchat/include/global.h — 客户端镜像 ErrorCodes、ReqId、MsgStatus
- server/common/include/Defer.h — RAII 延迟执行器
- server/ChatServer/src/CSession.cpp — Asio 异步 IO 异常捕获、连接生命周期管理
- server/ChatServer/src/MysqlDao.cpp — MySQL 异常捕获与连接池释放
- server/ChatServer/src/LogicSystem.cpp — 消息分发、认证校验、错误响应构造
- server/ChatServer/src/ChatGrpcClient.cpp — gRPC 重试、退避、状态码处理
- server/GateServer/include/const.h — Gate 层 ErrorCodes 定义