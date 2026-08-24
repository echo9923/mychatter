#pragma once
#include <functional>
#include "Defer.h"
#include "protocol_ids.h"

/**
 * @brief 服务端错误码枚举
 *
 * 数值唯一来源 proto/protocol_ids.h：通用错误引用 20xx 通用表
 * （与 GateServer/StatusServer/ChatServer 同一语义），文件/资源
 * 专属错误引用 21xx 资源表（ResourceServer 专属语义）。
 */
enum ErrorCodes {
	Success = llfc_proto::ERR_SUCCESS,
	Error_Json = llfc_proto::ERR_JSON,   //2001 Json解析错误
	RPCFailed = llfc_proto::ERR_RPC_FAILED,  //2002 RPC请求错误
	VarifyExpired = llfc_proto::ERR_VARIFY_EXPIRED,  //2003 验证码过期
	VarifyCodeErr = llfc_proto::ERR_VARIFY_CODE,  //2004 验证码错误
	UserExist = llfc_proto::ERR_USER_EXIST,       //2005 用户已经存在
	PasswdErr = llfc_proto::ERR_PASSWD,    //2006 密码错误
	EmailNotMatch = llfc_proto::ERR_EMAIL_NOT_MATCH,  //2007 邮箱不匹配
	PasswdUpFailed = llfc_proto::ERR_PASSWD_UP_FAILED,  //2008 更新密码失败
	PasswdInvalid = llfc_proto::ERR_PASSWD_INVALID,   //2009 密码更新失败
	TokenInvalid = llfc_proto::ERR_TOKEN_INVALID,   //2010 Token失效
	UidInvalid = llfc_proto::ERR_UID_INVALID,  //2011 uid无效
	FileNotExists = llfc_proto::RS_FILE_NOT_EXISTS, //2101 文件不存在
	FileSaveRedisFailed = llfc_proto::RS_FILE_SAVE_REDIS_FAILED, //2102 文件存储redis失败
	CreateFilePathFailed = llfc_proto::RS_CREATE_FILE_PATH_FAILED, //2103 文件路径创建失败
	FileWritePermissionFailed = llfc_proto::RS_FILE_WRITE_PERMISSION,  //2104 文件写权限不足
	FileReadPermissionFailed = llfc_proto::RS_FILE_READ_PERMISSION,  //2105 文件读权限不足
	FileSeqInvalid = llfc_proto::RS_FILE_SEQ_INVALID,     //2106 文件序列有误（原 seq 协议遗留，资源链路已改 offset，仅头像链路可能返回）
	FileOffsetInvalid = llfc_proto::RS_FILE_OFFSET_INVALID,   //2107 文件偏移量有误（超前于服务端已收位置，响应须带 server_offset）
	FileReadFailed = llfc_proto::RS_FILE_READ_FAILED,      //2108 文件读取失败
	RedisReadErr = llfc_proto::RS_REDIS_READ_ERR,        //2109 redis读取失败
	ServerIpErr = llfc_proto::RS_SERVER_IP_ERR,          //2110 server ip错误
	MsgIdErr = llfc_proto::RS_MSG_ID_ERR,             //2111 消息id错误
	FileHashMismatch = llfc_proto::RS_FILE_HASH_MISMATCH,     //2112 分片/整文件 SHA-256 校验失败
	FileSizeExceeded = llfc_proto::RS_FILE_SIZE_EXCEEDED,     //2113 资源超过类型上限
	ResourceNotReady = llfc_proto::RS_RESOURCE_NOT_READY,     //2114 resource_status != Ready 时请求下载
	ResourceForbidden = llfc_proto::RS_RESOURCE_FORBIDDEN,    //2115 请求者不是消息发送者或接收者
	ResourceStateInvalid = llfc_proto::RS_RESOURCE_STATE_INVALID, //2116 对已失败/已就绪资源的非法上传操作
};

enum MsgStatus {
	UN_READ = 0,  //对方未读
	SEND_FAILED = 1,  //发送失败
	READED = 2,  //对方已读
	//原 3=UN_UPLOAD 已废弃：资源生命周期一律读 chat_message.resource_status
};

/// 资源生命周期（对应 chat_message.resource_status，与 ChatServer 侧定义一致）
enum class ResourceStatus {
	Uploading = 0, //待上传（分片未收齐）
	Ready     = 1, //就绪（整文件 SHA-256 校验通过、已进同步流）
	Expired   = 2  //失败/过期（7 天清理标记或校验失败终态）
};

// Defer 已迁移至 common/include/Defer.h

#define MAX_LENGTH  1024*50
//头部总长度
#define HEAD_TOTAL_LEN 6
//头部消息类型长度
#define HEAD_TYPE_LEN 2
//头部数据长度
#define HEAD_DATA_LEN 4
#define MAX_RECVQUE  2000000
#define MAX_SENDQUE 2000000

//4个逻辑工作者
#define LOGIC_WORKER_COUNT 4
//4个文件工作者
#define FILE_WORKER_COUNT 4

//4个下载工作者
#define DOWN_LOAD_WORKER_COUNT	4


/**
 * @brief TCP消息类型枚举（ResourceServer 侧）
 *
 * 数值唯一来源 proto/protocol_ids.h（llfc_proto 命名空间）。
 * 15xx 资源域（1503/1504/1505 创建与通知由 ChatServer 处理）、
 * 16xx 头像域（旧 seq+MD5 协议保留）。
 */
enum MSG_TYPES {
	ID_UPLOAD_HEAD_ICON_REQ = llfc_proto::MSG_UPLOAD_HEAD_ICON_REQ,  //1601 上传头像请求
	ID_UPLOAD_HEAD_ICON_RSP = llfc_proto::MSG_UPLOAD_HEAD_ICON_RSP,  //1602 上传头像回复
	ID_DOWN_LOAD_FILE_REQ = llfc_proto::MSG_DOWN_LOAD_FILE_REQ,      //1603 下载文件请求（旧头像链路）
	ID_DOWN_LOAD_FILE_RSP = llfc_proto::MSG_DOWN_LOAD_FILE_RSP,      //1604 下载文件回复

	ID_RESOURCE_CHUNK_UPLOAD_REQ = llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_REQ,   //1507 上传资源分片（message_id/offset/chunk_sha256）
	ID_RESOURCE_CHUNK_UPLOAD_RSP = llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_RSP,   //1508 上传资源分片回复
	//1505 通用资源消息通知由 ChatServer 下发，ResourceServer 不处理
	ID_RESOURCE_UPLOAD_PROGRESS_REQ = llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_REQ, //1509 查询上传进度（返回服务端 .part 实际字节数）
	ID_RESOURCE_UPLOAD_PROGRESS_RSP = llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_RSP, //1510 查询上传进度回复
	//旧 1043/1044 续传分支已废弃删除：首传/续传统一 1507+1509
	ID_RESOURCE_DOWN_INFO_REQ = llfc_proto::MSG_RESOURCE_DOWN_INFO_REQ,     //1511 查询资源下载信息（含权限校验）
	ID_RESOURCE_DOWN_INFO_RSP = llfc_proto::MSG_RESOURCE_DOWN_INFO_RSP,     //1512 查询资源下载信息回复
	ID_RESOURCE_CHUNK_DOWN_REQ = llfc_proto::MSG_RESOURCE_CHUNK_DOWN_REQ,   //1513 按偏移量下载资源分片
	ID_RESOURCE_CHUNK_DOWN_RSP = llfc_proto::MSG_RESOURCE_CHUNK_DOWN_RSP,   //1514 按偏移量下载资源分片回复
	ID_RESOURCE_LOGIN_REQ = llfc_proto::MSG_RESOURCE_LOGIN_REQ,    //1501 资源服务登录鉴权请求
	ID_RESOURCE_LOGIN_RSP = llfc_proto::MSG_RESOURCE_LOGIN_RSP     //1502 资源服务登录鉴权回复
};

/// 上传/下载链的固定 worker 路由：同一 message_id 恒定落同一 worker，
/// 保证同一 .part 只被一个线程写、1509 进度查询与 1507 写入互不竞争
inline int ResourceWorkerIndex(long long message_id, int worker_count) {
	return static_cast<int>(static_cast<unsigned long long>(message_id)
		% static_cast<unsigned long long>(worker_count));
}

/// 资源大小上限默认值（字节），可被 config.ini [Resource] 段覆盖：
/// MaxImageSize / MaxFileSize
constexpr unsigned long long kDefaultMaxImageSize = 20ULL * 1024 * 1024;   // 20MB
constexpr unsigned long long kDefaultMaxFileSize = 100ULL * 1024 * 1024;   // 100MB
/// .part/未完成资源的保留时长（秒），过期由清理任务标记失败并回收磁盘
constexpr long long kResourceRetentionSeconds = 7LL * 24 * 3600;

#define USERIPPREFIX  "uip_"
#define IPCOUNTPREFIX  "ipcount_"
#define USER_BASE_INFO "ubaseinfo_"
#define NAME_INFO  "nameinfo_"

#define LOCK_PREFIX "lock_"
#define USER_SESSION_PREFIX "usession_"
/// Redis中存储用户登录令牌的键前缀，完整键为 "utoken_" + uid（由StatusServer在分配ChatServer时写入，TTL 86400s）
#define USERTOKENPREFIX "utoken_"
#define LOCK_COUNT "lockcount"

//分布式锁的持有时间
#define LOCK_TIME_OUT 10
//分布式锁的重试时间
#define ACQUIRE_TIME_OUT 5

//最大传输文件的大小
#define MAX_FILE_LEN 1024*32


