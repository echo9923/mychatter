// StatusServer.cpp : 此文件包含 "main" 函数。程序执行将在此开始并结束。
//

#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <string>
#include "const.h"
#include "ConfigMgr.h"
#include <hiredis/hiredis.h>
#include "RedisMgr.h"
#include <memory>
#include <thread>
#include <boost/asio.hpp>
#include "StatusServiceImpl.h"

bool RunServer() {
	// 获取全局配置管理器的单例实例（ConfigMgr::Inst() 返回静态单例引用）
	// 配置文件（如 config.ini）中的各项配置均通过该单例以 cfg[段名][键名] 的方式读取
	auto& cfg = ConfigMgr::Inst();

	// 从配置中拼接 gRPC 服务器的监听地址，形如 "0.0.0.0:50052"
	// cfg["StatusServer"]["Host"] 为监听 IP，cfg["StatusServer"]["Port"] 为监听端口
	std::string server_address(cfg["StatusServer"]["Host"] + ":" + cfg["StatusServer"]["Port"]);
	// 实例化状态服务实现类（实现了 gRPC 定义的状态查询接口，供其他服务调用）
	StatusServiceImpl service;

	// 创建 gRPC 服务器构建器，用于配置并构建服务器实例
	grpc::ServerBuilder builder;
	// 将服务器绑定到指定地址，InsecureServerCredentials 表示使用明文传输（非 TLS），
	// 在内部服务间调用时出于性能考虑常采用这种方式
	builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
	// 注册状态服务实现，使服务器能够处理客户端发来的状态查询请求
	builder.RegisterService(&service);

	// 构建并启动gRPC服务器
	// BuildAndStart() 返回指向服务器的智能指针；若构建失败会抛出异常
	std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
	// 打印启动日志，方便确认服务器实际监听的地址是否正确
	std::cout << "Server listening on " << server_address << std::endl;

	// 创建Boost.Asio的io_context
	// io_context 是 Asio 的事件循环核心，负责调度所有异步操作（此处仅用于监听系统信号）
	boost::asio::io_context io_context;
	// 创建signal_set用于捕获SIGINT（Ctrl+C）和SIGTERM（终止信号），
	// 目的是在进程收到退出信号时执行优雅关闭逻辑，而不是被强制结束
	boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);

	// 设置异步等待SIGINT信号
	// async_wait 注册一个异步回调：当收到 SIGINT/SIGTERM 信号时由 io_context 触发该回调
	// 注意通过引用捕获 server 与 io_context，回调执行时它们必须仍然有效（RunServer 作用域内成立）
	signals.async_wait([&server, &io_context](const boost::system::error_code& error, int signal_number) {
		// error 为空表示信号正常触发；若发生错误则不做处理
		if (!error) {
			std::cout << "Shutting down server..." << std::endl;
			server->Shutdown(); // 优雅地关闭服务器：停止接收新请求并等待正在处理的 RPC 完成
			io_context.stop(); // 停止io_context：让事件循环退出，结束信号监听的线程
		}
		});

	// 在单独的线程中运行io_context
	// io_context.run() 会阻塞运行事件循环，直到收到信号后才返回；
	// 放到独立线程并 detach（分离）后，主线程可以继续执行 server->Wait() 等待服务器退出
	std::thread([&io_context]() { io_context.run(); }).detach();

	// 等待服务器关闭
	// Wait() 会阻塞当前（主）线程，直到 Shutdown() 被调用且所有 RPC 处理完毕、服务器完全退出
	server->Wait();
	// 服务器正常退出后返回 true，由 main 判断是否需要清理资源
	return true;
}

int main(int argc, char** argv) {
	try {
		bool ok = RunServer();
		RedisMgr::GetInstance()->Close();
		if (!ok) {
			return EXIT_FAILURE;
		}
	}
	catch (std::exception const& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		RedisMgr::GetInstance()->Close();
		return EXIT_FAILURE;
	}

	return 0;
}
