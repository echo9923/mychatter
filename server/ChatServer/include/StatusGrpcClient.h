#pragma once
#include "const.h"
#include "Singleton.h"
#include "ConfigMgr.h"
#include "status.grpc.pb.h"
#include "status.pb.h"
#include <grpcpp/grpcpp.h>
using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::GetChatServerReq;
using message::GetChatServerRsp;
using message::LoginRsp;
using message::LoginReq;
using message::StatusService;

/**
 * @brief StatusServer gRPC 客户端（单例）
 * 
 * 用于向 StatusServer 发送 gRPC 请求，包括：
 * - 获取可用的ChatServer节点（负载均衡分配）
 * - 验证用户登录（Token校验、分配节点）
 */
class StatusGrpcClient :public Singleton<StatusGrpcClient>
{
	friend class Singleton<StatusGrpcClient>;
public:
	/// 析构函数
	~StatusGrpcClient() {

	}

	/**
	 * @brief 向StatusServer查询可用的ChatServer节点
	 * @param uid 用户ID（用于负载均衡分配）
	 * @return 包含分配的ChatServer节点信息的响应
	 */
	GetChatServerRsp GetChatServer(int uid);

	/**
	 * @brief 向StatusServer发起登录验证请求
	 * @param uid 用户ID
	 * @param token 用户登录Token
	 * @return 登录验证响应（包含分配的ChatServer节点、错误码等）
	 */
	LoginRsp Login(int uid, std::string token);

private:
	/// 私有构造函数，从配置文件读取StatusServer地址并建立共享gRPC通道
	StatusGrpcClient();
	/// 与StatusServer的共享gRPC通道
	std::shared_ptr<Channel> channel_;
	
};



