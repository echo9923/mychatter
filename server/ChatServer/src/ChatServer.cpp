// ChatServer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "LogicSystem.h"
#include <csignal>
#include <thread>
#include <mutex>
#include <ctime>
#include <nlohmann/json.hpp>
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "ChatServiceImpl.h"
#include "const.h"

using json = nlohmann::json;

using namespace std;
bool bstop = false;
std::condition_variable cond_quit;
std::mutex mutex_quit;

int main()
{
	auto& cfg = ConfigMgr::Inst();
	auto server_name = cfg["SelfServer"]["Name"];
	try {
		auto pool = std::make_shared<AsioIOServicePool>(std::thread::hardware_concurrency());

		// [Discovery] lease 上报配置：缺失/非法值回退默认（间隔 5s，TTL 15s）
		std::string ri_str = cfg["Discovery"]["ReportIntervalSeconds"];
		std::string ttl_str = cfg["Discovery"]["LeaseTtlSeconds"];
		int report_interval = (!ri_str.empty() && atoi(ri_str.c_str()) > 0) ? atoi(ri_str.c_str()) : 5;
		int lease_ttl = (!ttl_str.empty() && atoi(ttl_str.c_str()) > 0) ? atoi(ttl_str.c_str()) : 15;

		// 优雅退出：删除自己的 lease（chatserver:lease:<name>）并关闭 Redis 连接池
		Defer derfer ([server_name]() {
				RedisMgr::GetInstance()->Del("chatserver:lease:" + server_name);
				RedisMgr::GetInstance()->Close();
			});

		boost::asio::io_context  io_context;
		auto port_str = cfg["SelfServer"]["Port"];
		//创建Cserver智能指针
		auto pointer_server = std::make_shared<CServer>(io_context, atoi(port_str.c_str()), pool);
		//启动定时器
		pointer_server->StartTimer();

		// lease 上报定时器：启动立即上报一次已认证会话数，此后每 report_interval 秒用
		// SET chatserver:lease:<name> <count> EX <lease_ttl> 续租。回调以 weak_ptr 防悬挂；
		// 上报失败只记录 server name 与错误信息（不含 Redis 凭据），下一周期自然重试。
		auto lease_timer = std::make_shared<boost::asio::steady_timer>(io_context);
		std::weak_ptr<CServer> server_wp(pointer_server);
		std::function<void(const boost::system::error_code&)> report_lease;
		report_lease = [&server_name, report_interval, lease_ttl, server_wp, lease_timer, &report_lease](const boost::system::error_code& ec) {
			if (ec) {
				return;
			}
			auto sp = server_wp.lock();
			int auth_count = sp ? sp->GetAuthenticatedSessionCount() : 0;
			const std::string lease_key = "chatserver:lease:" + server_name;
			if (!RedisMgr::GetInstance()->SetEx(lease_key, lease_ttl, std::to_string(auth_count))) {
				std::cerr << "lease report failed for " << server_name << std::endl;
			}
			lease_timer->expires_after(std::chrono::seconds(report_interval));
			lease_timer->async_wait(report_lease);
		};
		report_lease(boost::system::error_code{});

		//定义一个GrpcServer

		std::string server_address(cfg["SelfServer"]["Host"] + ":" + cfg["SelfServer"]["RPCPort"]);
		ChatServiceImpl service;
		grpc::ServerBuilder builder;
		// 监听端口和添加服务
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service);
		service.RegisterServer(pointer_server);
		// 构建并启动gRPC服务器
		std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
		std::cout << "RPC Server listening on " << server_address << std::endl;

		//单独启动一个线程处理grpc服务
		std::thread  grpc_server_thread([&server]() {
				server->Wait();
			});

	
		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
		//计划1.6 优雅停机顺序：停止新 gRPC 投递 → 停止新 accept/read → 排空 logic worker → 停止 IO 池。
		//LogicSystem::Stop 幂等：静态析构再次调用不会重复 join。
		signals.async_wait([&io_context, pool, &server](auto, auto) {
			server->Shutdown();
			io_context.stop();
			LogicSystem::GetInstance()->Stop();
			pool->Stop();
			});
		
	
		//将Cserver注册给逻辑类方便以后清除连接
		LogicSystem::GetInstance()->SetServer(pointer_server);
		io_context.run();

		grpc_server_thread.join();
		pointer_server->StopTimer();
		return 0;
	}
	catch (std::exception& e) {
		std::cerr << "Exception: " << e.what() << endl;
	}

}

