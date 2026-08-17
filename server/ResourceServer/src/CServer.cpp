#include "CServer.h"
#include <iostream>
#include "AsioIOServicePool.h"

CServer::CServer(boost::asio::io_context& io_context, short port, std::shared_ptr<AsioIOServicePool> pool):_io_context(io_context), _port(port),
_acceptor(io_context, tcp::endpoint(tcp::v4(),port)), _pool(pool)
{
	cout << "Server start success, listen on port : " << _port << endl;
	StartAccept();
}

CServer::~CServer() {
	Stop();
	cout << "Server destruct listen on port : " << _port << endl;
}

void CServer::HandleAccept(shared_ptr<CSession> new_session, const boost::system::error_code& error){
	if (!error) {
		bool registered = false;
		{
			lock_guard<mutex> lock(_mutex);
			if (!_stopped.load()) {
				registered = _sessions.emplace(
					new_session->GetSessionId(), new_session).second;
			}
		}
		if (registered) {
			new_session->Start();
		}
		else {
			new_session->Close();
		}
	}
	else if (!_stopped.load()) {
		cout << "session accept failed, error is " << error.what() << endl;
	}

	if (!_stopped.load()) {
		StartAccept();
	}
}

void CServer::StartAccept() {
	if (_stopped.load()) {
		return;
	}
	auto &io_context = _pool->GetIOService();
	shared_ptr<CSession> new_session = make_shared<CSession>(io_context, this);
	_acceptor.async_accept(new_session->GetSocket(),
		[this, new_session](const boost::system::error_code& error) {
			HandleAccept(new_session, error);
		});
}

void CServer::RemoveSession(const std::shared_ptr<CSession>& session) {
	if (!session) {
		return;
	}
	lock_guard<mutex> lock(_mutex);
	auto it = _sessions.find(session->GetSessionId());
	if (it == _sessions.end() || it->second != session) {
		return;
	}
	_sessions.erase(it);
}

void CServer::Stop() {
	if (_stopped.exchange(true)) {
		return;
	}

	boost::system::error_code ignored;
	_acceptor.cancel(ignored);
	_acceptor.close(ignored);

	std::map<std::string, shared_ptr<CSession>> sessions_copy;
	{
		lock_guard<mutex> lock(_mutex);
		sessions_copy = _sessions;
	}
	for (const auto& entry : sessions_copy) {
		if (entry.second) {
			entry.second->Close();
		}
	}
}
