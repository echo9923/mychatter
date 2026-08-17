// ChatServer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "LogicSystem.h"
#include <csignal>
#include <thread>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "ChatServiceImpl.h"
#include "ChatServerRegistry.h"

using json = nlohmann::json;

int main()
{
	auto& cfg = ConfigMgr::Inst();
	auto server_name = cfg["SelfServer"]["Name"];
	auto tcp_host = cfg["SelfServer"]["RegisterHost"];
	if (tcp_host.empty()) tcp_host = cfg["SelfServer"]["Host"];
	auto tcp_port = cfg["SelfServer"]["RegisterPort"];
	if (tcp_port.empty()) tcp_port = cfg["SelfServer"]["Port"];
	auto rpc_port = cfg["SelfServer"]["RegisterRPCPort"];
	if (rpc_port.empty()) rpc_port = cfg["SelfServer"]["RPCPort"];
	const std::string registration_json = json({
		{ "name", server_name },
		{ "tcp_host", tcp_host },
		{ "tcp_port", tcp_port },
		{ "rpc_host", tcp_host },
		{ "rpc_port", rpc_port }
	}).dump();
	try {
		auto pool = std::make_shared<AsioIOServicePool>(std::thread::hardware_concurrency());

		// [Discovery] registration/lease refresh defaults to 2s with a 6s TTL.
		std::string ri_str = cfg["Discovery"]["ReportIntervalSeconds"];
		std::string ttl_str = cfg["Discovery"]["LeaseTtlSeconds"];
		int report_interval = (!ri_str.empty() && atoi(ri_str.c_str()) > 0) ? atoi(ri_str.c_str()) : 2;
		int lease_ttl = (!ttl_str.empty() && atoi(ttl_str.c_str()) > 0) ? atoi(ttl_str.c_str()) : 6;

		// 优雅退出：删除自己的 lease（chatserver:lease:<name>）并关闭 Redis 连接池
		Defer derfer ([server_name]() {
				RedisMgr::GetInstance()->HDel(llfc::kChatServerRegistryKey, server_name);
				RedisMgr::GetInstance()->Del(llfc::ChatServerLeaseKey(server_name));
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
		report_lease = [&server_name, &registration_json, report_interval, lease_ttl, server_wp, lease_timer, &report_lease](const boost::system::error_code& ec) {
			if (ec) {
				return;
			}
			auto sp = server_wp.lock();
			int auth_count = sp ? sp->GetAuthenticatedSessionCount() : 0;
			if (!RedisMgr::GetInstance()->HSet(
				llfc::kChatServerRegistryKey, server_name, registration_json)) {
				std::cerr << "registry report failed for " << server_name << std::endl;
			}
			if (!RedisMgr::GetInstance()->SetEx(llfc::ChatServerLeaseKey(server_name),
				lease_ttl, std::to_string(auth_count))) {
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
		signals.async_wait([&io_context, pool, &server, pointer_server](auto, auto) {
			pointer_server->Stop();
			server->Shutdown();
			io_context.stop();
			LogicSystem::GetInstance()->Stop();
			pool->Drain();
			pool->Stop();
			});
		
	
		io_context.run();

		grpc_server_thread.join();
		pointer_server->StopTimer();
		return 0;
	}
	catch (std::exception& e) {
		std::cerr << "Exception: " << e.what() << std::endl;
	}

}

