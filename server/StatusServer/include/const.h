#pragma once
#include <boost/beast/http.hpp>
#include <boost/beast.hpp>
#include <boost/asio.hpp>
#include <memory>
#include <iostream>
#include <unordered_map>
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#include "Singleton.h"
#include "Defer.h"
#include "protocol_ids.h"
#include <assert.h>
#include <queue>
#include <iostream>
#include <functional>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <string>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

// 错误码数值唯一来源 proto/protocol_ids.h 的 20xx 通用表
// （GateServer/StatusServer/ChatServer 共用同一语义表）
enum ErrorCodes {
	Success = llfc_proto::ERR_SUCCESS,
	Error_Json = llfc_proto::ERR_JSON,  //2001 Json解析错误
	RPCFailed = llfc_proto::ERR_RPC_FAILED,  //2002 RPC请求错误
	VarifyExpired = llfc_proto::ERR_VARIFY_EXPIRED, //2003 验证码过期
	VarifyCodeErr = llfc_proto::ERR_VARIFY_CODE, //2004 验证码错误
	UserExist = llfc_proto::ERR_USER_EXIST,       //2005 用户已经存在
	PasswdErr = llfc_proto::ERR_PASSWD,    //2006 密码错误
	EmailNotMatch = llfc_proto::ERR_EMAIL_NOT_MATCH,  //2007 邮箱不匹配
	PasswdUpFailed = llfc_proto::ERR_PASSWD_UP_FAILED,  //2008 更新密码失败
	PasswdInvalid = llfc_proto::ERR_PASSWD_INVALID,   //2009 密码更新失败
	TokenInvalid = llfc_proto::ERR_TOKEN_INVALID,   //2010 Token失效
	UidInvalid = llfc_proto::ERR_UID_INVALID,  //2011 uid无效
	NoAvailableChatServer = llfc_proto::ERR_NO_CHAT_SERVER, //2018 无可用ChatServer节点
};


// Defer 已迁移至 common/include/Defer.h

#define USERIPPREFIX  "uip_"
#define IPCOUNTPREFIX  "ipcount_"
#define USER_BASE_INFO "ubaseinfo_"
#define LOCK_COUNT "lockcount"
#define USERTOKENPREFIX "utoken_"

//分布式锁的持有时间
#define LOCK_TIME_OUT 10
//分布式锁的重试时间
#define ACQUIRE_TIME_OUT 5


