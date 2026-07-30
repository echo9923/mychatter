#pragma once
#include "const.h"
#include "Singleton.h"
#include "ConfigMgr.h"
#include <grpcpp/grpcpp.h> 
#include "chat.grpc.pb.h"
#include "chat.pb.h"
#include "const.h"
#include "data.h"
#include <nlohmann/json.hpp>
using json = nlohmann::json;

using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::AddFriendReq;
using message::AddFriendRsp;

using message::AuthFriendReq;
using message::AuthFriendRsp;

using message::ChatService;

using message::TextChatMsgReq;
using message::TextChatMsgRsp;
using message::TextChatData;

using message::KickUserReq;
using message::KickUserRsp;


/**
 * @brief ChatServer gRPC 客户端（单例）
 * 
 * 用于向其他 ChatServer 节点发送跨服 gRPC 请求。
 * 内部维护一个以节点名为键的共享 gRPC 通道映射，调用时按需创建 Stub。
 * 支持的操作包括：通知加好友、通知认证好友、转发文本聊天消息、踢人等。
 */
class ChatGrpcClient :public Singleton<ChatGrpcClient>
{
	friend class Singleton<ChatGrpcClient>;
public:
	/// 析构函数
	~ChatGrpcClient() {

	}

	/**
	 * @brief 通知目标ChatServer有用户收到了好友申请
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 添加好友请求消息（包含申请者、被申请者信息等）
	 * @return 添加好友响应消息
	 */
	AddFriendRsp NotifyAddFriend(std::string server_ip, const AddFriendReq& req);

	/**
	 * @brief 通知目标ChatServer有用户的好友申请已被认证通过
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 认证好友请求消息
	 * @return 认证好友响应消息
	 */
	AuthFriendRsp NotifyAuthFriend(std::string server_ip, const AuthFriendReq& req);

	/**
	 * @brief 从 Redis 中获取用户基本信息
	 * @param base_key Redis中用户信息的键前缀
	 * @param uid 用户ID
	 * @param userinfo [out] 输出参数，存储查询到的用户信息
	 * @return 是否成功获取到用户信息
	 */
	bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo);

	/**
	 * @brief 向目标ChatServer转发文本聊天消息
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 文本聊天消息请求
	 * @param rtvalue 原始JSON数据，用于日志或额外处理
	 * @return 文本聊天消息响应
	 */
	TextChatMsgRsp NotifyTextChatMsg(std::string server_ip, const TextChatMsgReq& req, const json& rtvalue);

	/**
	 * @brief 通知目标ChatServer踢出指定用户（用于跨服踢人/多端登录冲突）
	 * @param server_ip 目标ChatServer的IP地址
	 * @param req 踢人请求消息（包含被踢用户uid等）
	 * @return 踢人响应消息
	 */
	KickUserRsp NotifyKickUser(std::string server_ip, const KickUserReq& req);

private:
	/// 私有构造函数，从配置文件读取PeerServer配置并建立共享gRPC通道
	ChatGrpcClient();
	/// 共享gRPC通道映射表，键为目标节点的配置名，值为共享Channel
	unordered_map<std::string, std::shared_ptr<Channel>> _channels;
};



