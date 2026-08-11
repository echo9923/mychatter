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
	FileSeqInvalid = 1017,     //文件序列有误
	FileOffsetInvalid = 1018,   //文件偏移量有误
	FileReadFailed = 1019,      //文件读取失败
	RedisReadErr = 1020,        //redis读取失败
	ServerIpErr = 1021,          //server ip错误
	MsgIdErr = 1022,             //消息id错误
};

enum MsgStatus {
	UN_READ = 0,  //对方未读
	SEND_FAILED = 1,  //发送失败
	READED = 2,  //对方已读
	UN_UPLOAD = 3 //未上传完成
};

// Defer 已迁移至 common/include/Defer.h

#define MAX_LENGTH  1024*50
//头部总长度
#define HEAD_TOTAL_LEN 6
//头部id长度
#define HEAD_ID_LEN 2
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


enum MSG_IDS {
	ID_UPLOAD_HEAD_ICON_REQ = 1031,      //上传头像请求
	ID_UPLOAD_HEAD_ICON_RSP = 1032,      //上传头像回复
	ID_DOWN_LOAD_FILE_REQ = 1033,        //下载文件请求
	ID_DOWN_LOAD_FILE_RSP = 1034,        //下载文件回复

	ID_IMG_CHAT_UPLOAD_REQ = 1037,        //上传聊天图片资源
	ID_IMG_CHAT_UPLOAD_RSP = 1038,        //上传聊天图片资源回复
	ID_NOTIFY_IMG_CHAT_MSG_REQ = 1039, //通知用户图片聊天信息
	ID_FILE_INFO_SYNC_REQ = 1041,      //文件信息同步请求
	ID_FILE_INFO_SYNC_RSP = 1042,       //文件信息同步回复
	ID_IMG_CHAT_CONTINUE_UPLOAD_REQ = 1043,  //续传聊天图片资源请求
	ID_IMG_CHAT_CONTINUE_UPLOAD_RSP = 1044,  //续传聊天图片资源回复
	ID_IMG_CHAT_DOWN_INFO_SYNC_REQ = 1045,   //获取聊天图片下载的同步信息
	ID_IMG_CHAT_DOWN_INFO_SYNC_RSP = 1046,    //获取聊天图片下载的同步信息回包
	ID_IMG_CHAT_DOWN_REQ = 1047,    //聊天图片下载请求
	ID_IMG_CHAT_DOWN_RSP = 1048,     //聊天图片下载回复
	ID_RESOURCE_LOGIN_REQ = 1053,    //资源服务登录鉴权请求
	ID_RESOURCE_LOGIN_RSP = 1054     //资源服务登录鉴权回复
};

#define USERIPPREFIX  "uip_"
#define IPCOUNTPREFIX  "ipcount_"
#define USER_BASE_INFO "ubaseinfo_"
#define NAME_INFO  "nameinfo_"

#define LOCK_PREFIX "lock_"
#define USER_SESSION_PREFIX "usession_"
/// Redis中存储用户登录令牌的键前缀，完整键为 "utoken_" + uid（由StatusServer在分配ChatServer时写入，TTL 86400s）
#define USERTOKENPREFIX "utoken_"
#define LOCK_COUNT "lockcount"
/// Redis中离线消息待投递有序集合的键前缀，完整键为 "offline_msg:" + recv_uid，ZSET member/score 均为十进制 message_id
#define OFFLINE_MSG_PREFIX "offline_msg:"

//分布式锁的持有时间
#define LOCK_TIME_OUT 10
//分布式锁的重试时间
#define ACQUIRE_TIME_OUT 5

//最大传输文件的大小
#define MAX_FILE_LEN 1024*32


