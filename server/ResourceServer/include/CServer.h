#pragma once
#include <boost/asio.hpp>
#include "AsioIOServicePool.h"
#include "CSession.h"
#include <memory.h>
#include <map>
#include <atomic>
#include <mutex>
using namespace std;
using boost::asio::ip::tcp;
class CServer
{
public:
	CServer(boost::asio::io_context& io_context, short port, std::shared_ptr<AsioIOServicePool> pool);
	~CServer();
	void Stop();
private:
	friend class CSession;
	void RemoveSession(const std::shared_ptr<CSession>& session);
	void HandleAccept(shared_ptr<CSession>, const boost::system::error_code & error);
	void StartAccept();
	boost::asio::io_context &_io_context;
	short _port;
	tcp::acceptor _acceptor;
	std::shared_ptr<AsioIOServicePool> _pool;
	std::map<std::string, shared_ptr<CSession>> _sessions;
	std::mutex _mutex;
	std::atomic<bool> _stopped{false};
};

