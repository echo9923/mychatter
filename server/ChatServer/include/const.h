#pragma once
#include <functional>
#include <cstdint>
#include "Defer.h"
#include "protocol_ids.h"

/**
 * @brief 服务端统一错误码枚举
 *
 * 数值唯一来源 proto/protocol_ids.h 的 20xx 通用表
 * （GateServer/StatusServer/ChatServer 共用同一语义表），
 * 用于各服务间通信及向客户端返回错误信息，
 * 客户端根据错误码展示对应的提示信息。
 */
enum ErrorCodes {
	Success = llfc_proto::ERR_SUCCESS,             ///< 操作成功
	Error_Json = llfc_proto::ERR_JSON,             ///< JSON解析错误
	RPCFailed = llfc_proto::ERR_RPC_FAILED,        ///< RPC请求失败
	VarifyExpired = llfc_proto::ERR_VARIFY_EXPIRED, ///< 验证码已过期
	VarifyCodeErr = llfc_proto::ERR_VARIFY_CODE,   ///< 验证码错误
	UserExist = llfc_proto::ERR_USER_EXIST,        ///< 用户已存在（注册时）
	PasswdErr = llfc_proto::ERR_PASSWD,            ///< 密码错误
	EmailNotMatch = llfc_proto::ERR_EMAIL_NOT_MATCH, ///< 邮箱不匹配
	PasswdUpFailed = llfc_proto::ERR_PASSWD_UP_FAILED, ///< 更新密码失败
	PasswdInvalid = llfc_proto::ERR_PASSWD_INVALID,   ///< 密码无效/更新失败
	TokenInvalid = llfc_proto::ERR_TOKEN_INVALID,     ///< Token失效（登录凭证过期）
	UidInvalid = llfc_proto::ERR_UID_INVALID,         ///< 用户ID无效
	CREATE_CHAT_FAILED = llfc_proto::ERR_CREATE_CHAT_FAILED, ///< 创建聊天会话失败
	LOAD_CHAT_FAILED = llfc_proto::ERR_LOAD_CHAT_FAILED,   ///< 加载聊天记录失败
	MESSAGE_STORE_FAILED = llfc_proto::ERR_MESSAGE_STORE_FAILED, ///< 消息持久化失败（MySQL 写失败）
	RECIPIENT_OFFLINE = llfc_proto::ERR_RECIPIENT_OFFLINE,    ///< 目标用户当前不在线（无可用 session）
	SERVER_BUSY = llfc_proto::ERR_SERVER_BUSY,          ///< 服务端停机/队列拒绝，消息未入队未持久化
	MESSAGE_CONFLICT = llfc_proto::ERR_MESSAGE_CONFLICT,     ///< unique-id 相同但内容冲突，原消息不变
	NoAvailableChatServer = llfc_proto::ERR_NO_CHAT_SERVER, ///< 无可用 ChatServer 节点（所有 lease 缺失或过期）
	ResourceInvalid = llfc_proto::ERR_RESOURCE_INVALID,      ///< 资源元数据非法（文件名/SHA-256 格式/MIME 类型不合规）
	ResourceSizeExceeded = llfc_proto::ERR_RESOURCE_SIZE_EXCEEDED, ///< 资源超过类型上限（图片 20MB / 文件 100MB）
	FriendRequestNotFound = llfc_proto::ERR_FRIEND_REQUEST_NOT_FOUND,
	FriendRequestHandled = llfc_proto::ERR_FRIEND_REQUEST_HANDLED,
	AlreadyFriends = llfc_proto::ERR_ALREADY_FRIENDS,
	FriendActionInvalid = llfc_proto::ERR_FRIEND_ACTION_INVALID,
	SyncCursorInvalid = llfc_proto::ERR_SYNC_CURSOR_INVALID,
};


// Defer 已迁移至 common/include/Defer.h

/// 单次接收/发送缓冲区的最大字节数 (2KB)
#define MAX_LENGTH  1024*2
/// 消息头部总长度（字节），包含消息类型和数据长度
#define HEAD_TOTAL_LEN 4
/// 消息头部中消息类型占用的字节数
#define HEAD_TYPE_LEN 2
/// 消息头部中数据长度字段占用的字节数
#define HEAD_DATA_LEN 2
/// 接收队列最大容量，超过此值将拒绝接收新消息
#define MAX_RECVQUE  10000
/// 发送队列最大容量，超过此值将拒绝发送新消息
#define MAX_SENDQUE 1000


/**
 * @brief TCP消息类型枚举
 *
 * 定义客户端与ChatServer之间所有TCP消息的类型编号。
 * 消息协议格式：[2字节消息类型][2字节消息长度][数据体]
 * 数值唯一来源 proto/protocol_ids.h（llfc_proto 命名空间）。
 * 编号规则：百位=功能域（11连接/12好友/13聊天/14同步/15资源创建与通知），
 * 奇数=发起方（请求或通知），偶数=回包；请求/响应连续成对编号。
 */
enum MSG_TYPES {
	MSG_CHAT_LOGIN = llfc_proto::MSG_CHAT_LOGIN,          ///< 用户登录请求 1101
	MSG_CHAT_LOGIN_RSP = llfc_proto::MSG_CHAT_LOGIN_RSP,  ///< 用户登录响应 1102
	ID_SEARCH_USER_REQ = llfc_proto::MSG_SEARCH_USER_REQ, ///< 搜索用户请求 1201（按uid或用户名）
	ID_SEARCH_USER_RSP = llfc_proto::MSG_SEARCH_USER_RSP, ///< 搜索用户响应 1202
	ID_ADD_FRIEND_REQ = llfc_proto::MSG_ADD_FRIEND_REQ,   ///< 申请添加好友请求 1203
	ID_ADD_FRIEND_RSP  = llfc_proto::MSG_ADD_FRIEND_RSP,  ///< 申请添加好友响应 1204
	ID_HANDLE_FRIEND_REQ = llfc_proto::MSG_HANDLE_FRIEND_REQ, ///< 处理好友申请请求 1205
	ID_HANDLE_FRIEND_RSP = llfc_proto::MSG_HANDLE_FRIEND_RSP, ///< 处理好友申请响应 1206
	ID_TEXT_CHAT_MSG_REQ = llfc_proto::MSG_TEXT_CHAT_REQ, ///< 发送文本聊天消息请求 1301
	ID_TEXT_CHAT_MSG_RSP = llfc_proto::MSG_TEXT_CHAT_RSP, ///< 文本聊天消息发送响应 1302
	ID_NOTIFY_OFF_LINE_REQ = llfc_proto::MSG_NOTIFY_OFF_LINE, ///< 服务端通知用户被踢下线 1105
	ID_HEART_BEAT_REQ = llfc_proto::MSG_HEART_BEAT_REQ,   ///< 客户端心跳请求 1103
	ID_HEARTBEAT_RSP = llfc_proto::MSG_HEARTBEAT_RSP,     ///< 服务端心跳响应 1104
	ID_LOAD_CHAT_THREAD_REQ = llfc_proto::MSG_LOAD_CHAT_THREAD_REQ, ///< 加载聊天会话列表请求 1401
	ID_LOAD_CHAT_THREAD_RSP = llfc_proto::MSG_LOAD_CHAT_THREAD_RSP, ///< 加载聊天会话列表响应 1402
	ID_CREATE_PRIVATE_CHAT_REQ = llfc_proto::MSG_CREATE_PRIVATE_CHAT_REQ, ///< 创建私聊会话请求 1303
	ID_CREATE_PRIVATE_CHAT_RSP = llfc_proto::MSG_CREATE_PRIVATE_CHAT_RSP, ///< 创建私聊会话响应 1304
	ID_LOAD_CHAT_MSG_REQ = llfc_proto::MSG_LOAD_CHAT_MSG_REQ, ///< 加载历史聊天消息请求 1403（分页）
	ID_LOAD_CHAT_MSG_RSP = llfc_proto::MSG_LOAD_CHAT_MSG_RSP, ///< 加载历史聊天消息响应 1404
	ID_CREATE_RESOURCE_MSG_REQ = llfc_proto::MSG_CREATE_RESOURCE_REQ, ///< 创建资源消息请求 1503（图片/文件统一，元数据先行）
	ID_CREATE_RESOURCE_MSG_RSP = llfc_proto::MSG_CREATE_RESOURCE_RSP, ///< 创建资源消息响应 1504（返回 PENDING message_id）
	// 1505~1512 分片上传/进度查询/下载信息/分片下载由 ResourceServer 处理，不在此声明
	ID_SYNC_USER_MESSAGE_REQ = llfc_proto::MSG_SYNC_USER_MESSAGE_REQ, ///< 统一消息同步请求 1405
	ID_SYNC_USER_MESSAGE_RSP = llfc_proto::MSG_SYNC_USER_MESSAGE_RSP, ///< 统一消息同步响应 1406
	ID_NOTIFY_USER_MESSAGE = llfc_proto::MSG_NOTIFY_USER_MESSAGE ///< 统一实时消息通知 1701
};

/**
 * @brief 客户端请求消息ID到响应消息ID的映射
 *
 * 用于在无法正常进入 handler（如服务端停机/队列拒绝）时，仍能向客户端
 * 回送对应类型的错误响应。显式映射表比依赖“req+1”约定更安全，
 * 服务端用户消息统一通过 1701 通知，1702 留空；1105 仍为踢下线通知。
 * 表示无法回送，调用方应直接关闭连接。
 * @param req_id 客户端请求消息ID
 * @return 对应的响应消息ID；无映射时返回 0
 */
inline short ReqToRspId(short req_id) {
	switch (req_id) {
	case MSG_CHAT_LOGIN:               return MSG_CHAT_LOGIN_RSP;          // 1101 -> 1102
	case ID_SEARCH_USER_REQ:           return ID_SEARCH_USER_RSP;          // 1201 -> 1202
	case ID_ADD_FRIEND_REQ:            return ID_ADD_FRIEND_RSP;           // 1203 -> 1204
	case ID_HANDLE_FRIEND_REQ:         return ID_HANDLE_FRIEND_RSP;        // 1205 -> 1206
	case ID_TEXT_CHAT_MSG_REQ:         return ID_TEXT_CHAT_MSG_RSP;        // 1301 -> 1302
	case ID_HEART_BEAT_REQ:            return ID_HEARTBEAT_RSP;            // 1103 -> 1104
	case ID_LOAD_CHAT_THREAD_REQ:      return ID_LOAD_CHAT_THREAD_RSP;     // 1401 -> 1402
	case ID_CREATE_PRIVATE_CHAT_REQ:   return ID_CREATE_PRIVATE_CHAT_RSP;  // 1303 -> 1304
	case ID_LOAD_CHAT_MSG_REQ:         return ID_LOAD_CHAT_MSG_RSP;        // 1403 -> 1404
	case ID_CREATE_RESOURCE_MSG_REQ:   return ID_CREATE_RESOURCE_MSG_RSP;   // 1503 -> 1504
	case ID_SYNC_USER_MESSAGE_REQ:    return ID_SYNC_USER_MESSAGE_RSP;    // 1405 -> 1406
	default:                           return 0;
	}
}

/// Redis中存储用户IP地址的键前缀，完整键为 "uip_" + uid
#define USERIPPREFIX  "uip_"
/// Redis中存储各ChatServer节点IP连接数的键前缀，用于负载均衡
#define IPCOUNTPREFIX  "ipcount_"
/// Redis中存储用户基本信息的Hash键前缀，完整键为 "ubaseinfo_" + uid
#define USER_BASE_INFO "ubaseinfo_"
/// Redis中存储用户名到uid映射的键前缀，完整键为 "nameinfo_" + name
#define NAME_INFO  "nameinfo_"
/// Redis中分布式锁的键前缀，完整键为 "lock_" + 资源名
#define LOCK_PREFIX "lock_"
/// Redis中存储用户会话ID的键前缀，用于踢人逻辑判断
#define USER_SESSION_PREFIX "usession_"
/// Redis中存储分布式锁计数器的键名
#define LOCK_COUNT "lockcount"
/// Redis中存储用户访问令牌的键前缀，完整键为 "utoken_" + uid（由 StatusServer 密码登录后 SetEx 86400s 写入，底层使用 SET ... EX，ChatServer 登录时 Get 校验）
#define USERTOKENPREFIX "utoken_"

/// 分布式锁的持有超时时间（秒），超时后锁自动释放防止死锁
#define LOCK_TIME_OUT 10
/// 分布式锁的获取重试超时时间（秒），超过此时间未获取到锁则放弃
#define ACQUIRE_TIME_OUT 5

/// 资源大小上限默认值（字节），可被 config.ini [Resource] 段覆盖：
/// MaxImageSize / MaxFileSize
constexpr std::uint64_t kDefaultMaxImageSize = 20ULL * 1024 * 1024;   // 20MB
constexpr std::uint64_t kDefaultMaxFileSize = 100ULL * 1024 * 1024;   // 100MB


