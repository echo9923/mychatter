#pragma once
#include <functional>
#include "Defer.h"


enum ErrorCodes {
	Success = 0,
	Error_Json = 1001,  //Json解析错误
	RPCFailed = 1002,  //RPC请求错误
	VarifyExpired = 1003, //验证码过期
	VarifyCodeErr = 1004, //验证码错误
	UserExist = 1005,       //用户已经存在
	PasswdErr = 1006,    //密码错误
	EmailNotMatch = 1007,  //邮箱不匹配
	PasswdUpFailed = 1008,  //更新密码失败
	PasswdInvalid = 1009,   //密码更新失败
	TokenInvalid = 1010,   //Token失效
	UidInvalid = 1011,  //uid无效
	FileNotExists = 1012, //文件不存在
	FileSaveRedisFailed = 1013, //文件存储redis失败
	CreateFilePathFailed = 1014, //文件路径创建失败
	FileWritePermissionFailed = 1015,  //文件写权限不足
	FileReadPermissionFailed = 1016,  //文件读权限不足
	FileSeqInvalid = 1017,     //文件序列有误（原 seq 协议遗留，资源链路已改 offset，仅头像链路可能返回）
	FileOffsetInvalid = 1018,   //文件偏移量有误（超前于服务端已收位置，响应须带 server_offset）
	FileReadFailed = 1019,      //文件读取失败
	RedisReadErr = 1020,        //redis读取失败
	ServerIpErr = 1021,          //server ip错误
	MsgIdErr = 1022,             //消息id错误
	FileHashMismatch = 1023,     //分片/整文件 SHA-256 校验失败
	FileSizeExceeded = 1024,     //资源超过类型上限
	ResourceNotReady = 1025,     //resource_status != Ready 时请求下载
	ResourceForbidden = 1026,    //请求者不是消息发送者或接收者
	ResourceStateInvalid = 1027, //对已失败/已就绪资源的非法上传操作
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


enum MSG_TYPES {
	ID_UPLOAD_HEAD_ICON_REQ = 1031,      //上传头像请求
	ID_UPLOAD_HEAD_ICON_RSP = 1032,      //上传头像回复
	ID_DOWN_LOAD_FILE_REQ = 1033,        //下载文件请求
	ID_DOWN_LOAD_FILE_RSP = 1034,        //下载文件回复

	ID_RESOURCE_CHUNK_UPLOAD_REQ = 1037,   //上传资源分片（message_id/offset/chunk_sha256）
	ID_RESOURCE_CHUNK_UPLOAD_RSP = 1038,   //上传资源分片回复
	//1039 由 ChatServer 下发（通用资源消息通知），ResourceServer 不处理
	ID_RESOURCE_UPLOAD_PROGRESS_REQ = 1041, //查询上传进度（返回服务端 .part 实际字节数）
	ID_RESOURCE_UPLOAD_PROGRESS_RSP = 1042, //查询上传进度回复
	//1043/1044 续传分支已废弃删除：首传/续传统一 1037+1041
	ID_RESOURCE_DOWN_INFO_REQ = 1045,     //查询资源下载信息（含权限校验）
	ID_RESOURCE_DOWN_INFO_RSP = 1046,     //查询资源下载信息回复
	ID_RESOURCE_CHUNK_DOWN_REQ = 1047,    //按偏移量下载资源分片
	ID_RESOURCE_CHUNK_DOWN_RSP = 1048,    //按偏移量下载资源分片回复
	ID_RESOURCE_LOGIN_REQ = 1053,    //资源服务登录鉴权请求
	ID_RESOURCE_LOGIN_RSP = 1054     //资源服务登录鉴权回复
};

/// 上传/下载链的固定 worker 路由：同一 message_id 恒定落同一 worker，
/// 保证同一 .part 只被一个线程写、1041 进度查询与 1037 写入互不竞争
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


