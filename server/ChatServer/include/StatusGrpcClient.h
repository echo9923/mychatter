#pragma once
#include "const.h"
#include "Singleton.h"
#include "ConfigMgr.h"
#include "status.grpc.pb.h"
#include "status.pb.h"
#include <grpcpp/grpcpp.h>
#include <queue>
#include <condition_variable>
using grpc::Channel;
using grpc::Status;
using grpc::ClientContext;

using message::GetChatServerReq;
using message::GetChatServerRsp;
using message::LoginRsp;
using message::LoginReq;
using message::StatusService;

/**
 * @brief StatusServer gRPC 连接池
 * 
 * 管理与 StatusServer 的 gRPC 连接，采用对象池模式复用 Stub 连接。
 * ChatServer 通过该连接池向 StatusServer 查询可用节点、验证登录等。
 */
class StatusConPool {
public:
	/**
	 * @brief 构造连接池，预创建指定数量的gRPC Stub连接
	 * @param poolSize 连接池大小
	 * @param host StatusServer的IP地址
	 * @param port StatusServer的gRPC端口号
	 */
	StatusConPool(size_t poolSize, std::string host, std::string port)
		: poolSize_(poolSize), host_(host), port_(port), b_stop_(false) {
		for (size_t i = 0; i < poolSize_; ++i) {

			std::shared_ptr<Channel> channel = grpc::CreateChannel(host + ":" + port,
				grpc::InsecureChannelCredentials());

			connections_.push(StatusService::NewStub(channel));
		}
	}

	/// 析构函数，关闭连接池并清空所有连接
	~StatusConPool() {
		std::lock_guard<std::mutex> lock(mutex_);
		Close();
		while (!connections_.empty()) {
			connections_.pop();
		}
	}

	/**
	 * @brief 从连接池获取一个可用的gRPC Stub连接（阻塞等待）
	 * @return StatusService::Stub 智能指针，若池已停止则返回nullptr
	 */
	std::unique_ptr<StatusService::Stub> getConnection() {
		std::unique_lock<std::mutex> lock(mutex_);
		cond_.wait(lock, [this] {
			if (b_stop_) {
				return true;
			}
			return !connections_.empty();
			});
		//如果停止则直接返回空指针
		if (b_stop_) {
			return  nullptr;
		}
		auto context = std::move(connections_.front());
		connections_.pop();
		return context;
	}

	/**
	 * @brief 将使用完毕的gRPC Stub连接归还到连接池
	 * @param context 要归还的Stub连接
	 */
	void returnConnection(std::unique_ptr<StatusService::Stub> context) {
		std::lock_guard<std::mutex> lock(mutex_);
		if (b_stop_) {
			return;
		}
		connections_.push(std::move(context));
		cond_.notify_one();
	}

	/// 关闭连接池，唤醒所有等待线程使其退出
	void Close() {
		b_stop_ = true;
		cond_.notify_all();
	}

private:
	/// 停止标志，为true时连接池不再提供连接
	atomic<bool> b_stop_;
	/// 连接池容量
	size_t poolSize_;
	/// StatusServer主机地址
	std::string host_;
	/// StatusServer gRPC端口
	std::string port_;
	/// gRPC Stub连接队列
	std::queue<std::unique_ptr<StatusService::Stub>> connections_;
	/// 互斥锁，保护连接队列的线程安全
	std::mutex mutex_;
	/// 条件变量，无可用连接时阻塞等待
	std::condition_variable cond_;
};

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
	/// 私有构造函数，从配置文件读取StatusServer地址并初始化连接池
	StatusGrpcClient();
	/// StatusServer gRPC连接池实例
	std::unique_ptr<StatusConPool> pool_;
	
};



