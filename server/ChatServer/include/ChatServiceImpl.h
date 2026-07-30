#pragma once
#include <grpcpp/grpcpp.h>
#include "chat.grpc.pb.h"
#include "chat.pb.h"
#include <mutex>
#include "data.h"
#include "CServer.h"
#include <memory>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
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
using message::NotifyChatImgReq;


/**
 * @brief ChatService gRPC 服务实现类
 * 
 * 实现由 proto 文件定义的 ChatService gRPC 接口，作为本 ChatServer 的 gRPC 服务端。
 * 接收来自其他 ChatServer 或 ResourceServer 的跨服 gRPC 调用，包括：
 * - 好友申请通知
 * - 好友认证通知
 * - 文本聊天消息转发
 * - 踢人请求
 * - 图片聊天消息通知
 */
class ChatServiceImpl final: public ChatService::Service
{
public:
	/// 构造函数，初始化gRPC服务实现
	ChatServiceImpl();

	/**
	 * @brief 处理加好友通知请求，将好友申请推送给在线目标用户
	 * @param context gRPC服务端上下文
	 * @param request 添加好友请求（包含申请者uid、目标用户uid、申请描述等）
	 * @param reply [out] 添加好友响应
	 * @return gRPC调用状态
	 */
	Status NotifyAddFriend(ServerContext* context, const AddFriendReq* request,
		AddFriendRsp* reply) override;

	/**
	 * @brief 处理好友认证通知请求，将认证结果推送给在线申请者
	 * @param context gRPC服务端上下文
	 * @param request 认证好友请求（包含认证者uid、申请者uid、是否同意等）
	 * @param response [out] 认证好友响应
	 * @return gRPC调用状态
	 */
	Status NotifyAuthFriend(ServerContext* context, 
		const AuthFriendReq* request, AuthFriendRsp* response) override;

	/**
	 * @brief 处理文本聊天消息转发请求，将消息推送给在线接收者
	 * @param context gRPC服务端上下文
	 * @param request 文本聊天消息请求（包含发送者、接收者、消息内容等）
	 * @param response [out] 文本聊天消息响应
	 * @return gRPC调用状态
	 */
	Status NotifyTextChatMsg(::grpc::ServerContext* context, 
		const TextChatMsgReq* request, TextChatMsgRsp* response) override;

	/**
	 * @brief 从 Redis 获取用户基本信息
	 * @param base_key Redis中用户信息的键前缀
	 * @param uid 用户ID
	 * @param userinfo [out] 输出参数，存储查询到的用户信息
	 * @return 是否成功获取
	 */
	bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo);

	/**
	 * @brief 处理踢人请求，断开指定用户的TCP连接并清理会话
	 * @param context gRPC服务端上下文
	 * @param request 踢人请求（包含被踢用户uid）
	 * @param response [out] 踢人响应
	 * @return gRPC调用状态
	 */
	Status NotifyKickUser(::grpc::ServerContext* context,
		const KickUserReq* request, KickUserRsp* response) override;

	/**
	 * @brief 注册CServer实例，使gRPC服务能够访问TCP服务器层的会话管理功能
	 * @param pServer CServer共享指针
	 */
	void RegisterServer(std::shared_ptr<CServer> pServer);

	/**
	 * @brief 处理ResourceServer发送的图片聊天通知，将图片消息推送给在线接收者
	 * @param context gRPC服务端上下文
	 * @param request 图片聊天通知请求（包含图片URL、发送者、接收者等）
	 * @param response [out] 图片聊天通知响应
	 * @return gRPC调用状态
	 */
	virtual ::grpc::Status NotifyChatImgMsg(::grpc::ServerContext* context, const ::message::NotifyChatImgReq* request, ::message::NotifyChatImgRsp* response) override;

private:
	/// CServer实例指针，用于访问在线会话、向客户端推送消息
	std::shared_ptr<CServer> _p_server;
};

