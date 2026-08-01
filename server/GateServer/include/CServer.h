#pragma once
#include <string>
#include <memory>
#include "const.h"
#include "AsioIOServicePool.h"

class CServer:public std::enable_shared_from_this<CServer>
{
public:
	CServer(boost::asio::io_context& ioc, unsigned short& port,
		std::shared_ptr<AsioIOServicePool> pool);
	void Start();
private:
	tcp::acceptor  _acceptor;
	net::io_context& _ioc;
	std::shared_ptr<AsioIOServicePool> _pool;

};

