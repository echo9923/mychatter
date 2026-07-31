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
	auto server_register_host = cfg["SelfServer"]["RegisterHost"];
	if (server_register_host.empty()) {
		server_register_host = cfg["SelfServer"]["Host"];
	}
	auto server_port = cfg["SelfServer"]["Port"];
	try {
		auto pool = AsioIOServicePool::GetInstance();
		//注册节点元数据到Redis（服务注册）
		json server_info;
		server_info["name"] = server_name;
		server_info["host"] = server_register_host;
		server_info["port"] = server_port;
		RedisMgr::GetInstance()->HSet(CHATSERVER_INFO_KEY, server_name, server_info.dump());

		//立即发送一次心跳
		std::string hb_key = CHATSERVER_HEARTBEAT_PREFIX + server_name;
		RedisMgr::GetInstance()->SetWithExpire(hb_key, std::to_string(std::time(nullptr)), HEARTBEAT_TTL_SECONDS);

		//将登录数设置为0
		RedisMgr::GetInstance()->HSet(LOGIN_COUNT, server_name, "0");
		Defer derfer ([server_name]() {
				RedisMgr::GetInstance()->HDel(LOGIN_COUNT, server_name);
				RedisMgr::GetInstance()->Del(CHATSERVER_HEARTBEAT_PREFIX + server_name);
				RedisMgr::GetInstance()->HDel(CHATSERVER_INFO_KEY, server_name);
				RedisMgr::GetInstance()->Close();
			});

		boost::asio::io_context  io_context;
		auto port_str = cfg["SelfServer"]["Port"];
		//创建Cserver智能指针
		auto pointer_server = std::make_shared<CServer>(io_context, atoi(port_str.c_str()));
		//启动定时器
		pointer_server->StartTimer();
		pointer_server->StartHeartbeat();

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

