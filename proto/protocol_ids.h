#pragma once
// =============================================================================
// llfcchat 全项目唯一协议号来源（客户端 C++11 / 服务端 C++17 / 集成测试共用）
//
// 编号规则（三句话）：
//   1. 百位 = 功能域：10 账户 / 11 连接保活 / 12 好友 / 13 聊天 /
//      14 历史同步 / 15 资源 / 16 头像 / 17 统一用户消息；18xx 预留群聊
//   2. 奇数 = 发起方（请求或服务端推送通知），偶数 = 回包
//   3. 同一动作连号：REQ → RSP → NOTIFY（带推送的动作占 3 个号，
//      下一动作跳到下一个奇数，故全表仅 1206/1304/1506 为空号）
//
// 错误码独立于消息 ID：20xx 通用表（Gate/Status/Chat 同一语义），
// 21xx 资源文件表（ResourceServer 专属）。1xxx 与 2xxx 永不混用。
//
// 各端在自身 const.h / global.h 中保留本地枚举壳（符号名不变、业务代码
// 零改动），壳内数值一律引用本文件常量。新增/修改协议号只改这一处。
// =============================================================================

namespace llfc_proto {

// ===== 消息 ID ==============================================================
// TCP 帧头 2 字节 msg_id；ChatServer 校验 msg_id <= 2048，新号不得越界。
enum MsgId {
	// --- 10xx 账户域（GateServer HTTP；仅作客户端内部回调路由 id，不上 TCP 线）
	MSG_GET_VARIFY_CODE = 1001, ///< 获取邮箱验证码（POST /get_varifycode）
	MSG_REG_USER        = 1002, ///< 注册用户（POST /user_register）
	MSG_RESET_PWD       = 1003, ///< 重置密码（POST /reset_pwd）
	MSG_LOGIN_USER      = 1004, ///< 用户登录（POST /user_login）
	MSG_REASSIGN_CHAT   = 1005, ///< 断线重连复用 token 换新 ChatServer（POST /reassign_chat）

	// --- 11xx 连接与保活域（ChatServer TCP）
	MSG_CHAT_LOGIN      = 1101, ///< 登录聊天服务器请求
	MSG_CHAT_LOGIN_RSP  = 1102, ///< 登录聊天服务器响应
	MSG_HEART_BEAT_REQ  = 1103, ///< 心跳请求
	MSG_HEARTBEAT_RSP   = 1104, ///< 心跳响应
	MSG_NOTIFY_OFF_LINE = 1105, ///< 服务端通知用户被踢下线（gRPC NotifyKickUser 触发）

	// --- 12xx 好友域（ChatServer TCP）
	MSG_SEARCH_USER_REQ     = 1201, ///< 搜索用户请求（按 uid 或用户名）
	MSG_SEARCH_USER_RSP     = 1202, ///< 搜索用户响应
	MSG_ADD_FRIEND_REQ      = 1203, ///< 申请添加好友请求
	MSG_ADD_FRIEND_RSP      = 1204, ///< 申请添加好友响应
	MSG_HANDLE_FRIEND_REQ   = 1207, ///< 处理好友申请请求（action=accept/reject）
	MSG_HANDLE_FRIEND_RSP   = 1208, ///< 处理好友申请响应

	// --- 13xx 聊天域（ChatServer TCP）
	MSG_TEXT_CHAT_REQ          = 1301, ///< 发送文本聊天消息请求
	MSG_TEXT_CHAT_RSP          = 1302, ///< 文本聊天消息发送响应
	MSG_CREATE_PRIVATE_CHAT_REQ = 1305, ///< 创建私聊会话请求
	MSG_CREATE_PRIVATE_CHAT_RSP = 1306, ///< 创建私聊会话响应

	// --- 14xx 历史与同步域（ChatServer TCP）
	MSG_LOAD_CHAT_THREAD_REQ = 1401, ///< 加载聊天会话列表请求
	MSG_LOAD_CHAT_THREAD_RSP = 1402, ///< 加载聊天会话列表响应
	MSG_LOAD_CHAT_MSG_REQ    = 1403, ///< 加载历史聊天消息请求（分页）
	MSG_LOAD_CHAT_MSG_RSP    = 1404, ///< 加载历史聊天消息响应
	MSG_SYNC_USER_MESSAGE_REQ = 1405, ///< 按接收者 recv_seq 游标增量同步统一消息
	MSG_SYNC_USER_MESSAGE_RSP = 1406, ///< 统一消息增量同步响应

	// --- 15xx 资源域（ResourceServer TCP；1503/1504 创建元数据由 ChatServer 处理）
	MSG_RESOURCE_LOGIN_REQ   = 1501, ///< 资源服务器登录鉴权请求（每连接一次，Gate token）
	MSG_RESOURCE_LOGIN_RSP   = 1502, ///< 资源服务器登录鉴权响应
	MSG_CREATE_RESOURCE_REQ  = 1503, ///< 创建资源消息请求（图片/文件统一，元数据先行）
	MSG_CREATE_RESOURCE_RSP  = 1504, ///< 创建资源消息响应（返回 message_id，resource_status=0）
	MSG_RESOURCE_CHUNK_UPLOAD_REQ     = 1507, ///< 上传资源分片请求（message_id/offset/chunk_sha256）
	MSG_RESOURCE_CHUNK_UPLOAD_RSP     = 1508, ///< 上传资源分片响应（带 server_offset/resource_status）
	MSG_RESOURCE_UPLOAD_PROGRESS_REQ  = 1509, ///< 查询上传进度请求（返回服务端 .part 实际字节数）
	MSG_RESOURCE_UPLOAD_PROGRESS_RSP  = 1510, ///< 查询上传进度响应
	MSG_RESOURCE_DOWN_INFO_REQ        = 1511, ///< 查询资源下载信息请求（含权限校验）
	MSG_RESOURCE_DOWN_INFO_RSP        = 1512, ///< 查询资源下载信息响应
	MSG_RESOURCE_CHUNK_DOWN_REQ       = 1513, ///< 按偏移量下载资源分片请求
	MSG_RESOURCE_CHUNK_DOWN_RSP       = 1514, ///< 按偏移量下载资源分片响应（随片下发 SHA-256）

	// --- 16xx 头像域（ResourceServer TCP，旧 seq+MD5 协议保留，与资源新协议隔离）
	MSG_UPLOAD_HEAD_ICON_REQ = 1601, ///< 上传头像请求
	MSG_UPLOAD_HEAD_ICON_RSP = 1602, ///< 上传头像响应
	MSG_DOWN_LOAD_FILE_REQ   = 1603, ///< 下载文件请求（旧头像下载链路）
	MSG_DOWN_LOAD_FILE_RSP   = 1604, ///< 下载文件响应

	// --- 17xx 统一用户消息域（ChatServer TCP）
	MSG_NOTIFY_USER_MESSAGE  = 1701, ///< 服务端推送完整统一消息；客户端不回 ACK
};

// ===== 错误码：20xx 通用表 ==================================================
// GateServer / StatusServer / ChatServer 共用同一张语义表（Gate/Status 为子集），
// 经 TCP/HTTP JSON 的 error 字段传输。
enum CommonErrCode {
	ERR_SUCCESS = 0,                ///< 操作成功
	ERR_JSON    = 2001,             ///< JSON 解析错误
	ERR_RPC_FAILED        = 2002,   ///< RPC 请求失败
	ERR_VARIFY_EXPIRED    = 2003,   ///< 验证码已过期
	ERR_VARIFY_CODE       = 2004,   ///< 验证码错误
	ERR_USER_EXIST        = 2005,   ///< 用户已存在（注册时）
	ERR_PASSWD            = 2006,   ///< 密码错误
	ERR_EMAIL_NOT_MATCH   = 2007,   ///< 邮箱不匹配
	ERR_PASSWD_UP_FAILED  = 2008,   ///< 更新密码失败
	ERR_PASSWD_INVALID    = 2009,   ///< 密码无效/更新失败
	ERR_TOKEN_INVALID     = 2010,   ///< Token 失效（登录凭证过期）
	ERR_UID_INVALID       = 2011,   ///< 用户 ID 无效
	ERR_CREATE_CHAT_FAILED    = 2012, ///< 创建聊天会话失败
	ERR_LOAD_CHAT_FAILED      = 2013, ///< 加载聊天记录失败
	ERR_MESSAGE_STORE_FAILED  = 2014, ///< 消息持久化失败（transient，继续重传）
	ERR_RECIPIENT_OFFLINE     = 2015, ///< 目标用户当前不在线（无可用 session）
	ERR_SERVER_BUSY           = 2016, ///< 服务端停机/队列拒绝（transient，继续重传）
	ERR_MESSAGE_CONFLICT      = 2017, ///< unique-id 相同但内容冲突（permanent，停止重传）
	ERR_NO_CHAT_SERVER        = 2018, ///< 无可用 ChatServer 节点（所有 lease 缺失或过期）
	ERR_RESOURCE_INVALID      = 2019, ///< 资源元数据非法（文件名/SHA-256/MIME 不合规，permanent）
	ERR_RESOURCE_SIZE_EXCEEDED = 2020, ///< 资源超过类型上限（图片 20MB/文件 100MB，permanent）
	ERR_FRIEND_REQUEST_NOT_FOUND = 2021, ///< 好友申请不存在或不属于当前处理人
	ERR_FRIEND_REQUEST_HANDLED   = 2022, ///< 好友申请已被其他动作处理
	ERR_ALREADY_FRIENDS          = 2023, ///< 双方已经是好友
	ERR_FRIEND_ACTION_INVALID    = 2024, ///< 好友申请处理动作不是 accept/reject
	ERR_SYNC_CURSOR_INVALID      = 2025, ///< 客户端 recv_seq 游标超过服务端序号头
};

// ===== 错误码：21xx 资源文件表 ==============================================
// ResourceServer 专属语义（历史上与通用表共用 1012~1027 数值段且语义冲突，
// 如 1018 在通用表是"无可用节点"、在资源表是"偏移超前"；2xxx 起彻底分离）。
// ResourceServer 的通用错误（JSON 解析等）仍使用 20xx 通用表。
enum ResourceErrCode {
	RS_FILE_NOT_EXISTS         = 2101, ///< 文件不存在
	RS_FILE_SAVE_REDIS_FAILED  = 2102, ///< 文件存储 Redis 失败
	RS_CREATE_FILE_PATH_FAILED = 2103, ///< 文件路径创建失败
	RS_FILE_WRITE_PERMISSION   = 2104, ///< 文件写权限不足
	RS_FILE_READ_PERMISSION    = 2105, ///< 文件读权限不足
	RS_FILE_SEQ_INVALID        = 2106, ///< 文件序列有误（原 seq 协议遗留，仅头像链路可能返回）
	RS_FILE_OFFSET_INVALID     = 2107, ///< 分片偏移超前（响应须带 server_offset 供对齐重发）
	RS_FILE_READ_FAILED        = 2108, ///< 文件读取失败
	RS_REDIS_READ_ERR          = 2109, ///< Redis 读取失败
	RS_SERVER_IP_ERR           = 2110, ///< server ip 错误
	RS_MSG_ID_ERR              = 2111, ///< 消息不存在（permanent，标失败）
	RS_FILE_HASH_MISMATCH      = 2112, ///< 分片/整文件 SHA-256 校验失败（重传该片/整文件）
	RS_FILE_SIZE_EXCEEDED      = 2113, ///< 资源超过类型上限
	RS_RESOURCE_NOT_READY      = 2114, ///< resource_status != Ready 时请求下载（稍后重试）
	RS_RESOURCE_FORBIDDEN      = 2115, ///< 请求者不是消息发送者或接收者（permanent）
	RS_RESOURCE_STATE_INVALID  = 2116, ///< 对已失败/已就绪资源的非法上传操作（permanent）
};

} // namespace llfc_proto
