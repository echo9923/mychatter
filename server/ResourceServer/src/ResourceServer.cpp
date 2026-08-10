#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <boost/asio.hpp>
#include "AsioIOServicePool.h"
#include "CServer.h"
#include "ConfigMgr.h"

int main()
{
	auto& cfg = ConfigMgr::Inst();

	std::shared_ptr<AsioIOServicePool> pool;
	try {
		pool = std::make_shared<AsioIOServicePool>(std::thread::hardware_concurrency());

		boost::asio::io_context  io_context;
		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
		signals.async_wait([&io_context, pool](auto, auto) {
			io_context.stop();
			pool->Stop();
			});
		auto port_str = cfg["SelfServer"]["Port"];
		CServer s(io_context, atoi(port_str.c_str()), pool);
		io_context.run();
	}
	catch (std::exception& e) {
		if (pool) {
			pool->Stop();
		}
		std::cerr << "Exception: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
