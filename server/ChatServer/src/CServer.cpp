#include "CServer.h"
#include <iostream>
#include <ctime>
#include <cstdlib>
#include "AsioIOServicePool.h"
#include "ConfigMgr.h"

namespace {
//心跳扫描间隔可配置（[Heartbeat] SweepIntervalSeconds），缺省 60 秒
int HeartbeatSweepSeconds() {
	auto sweep_str = ConfigMgr::Inst()["Heartbeat"]["SweepIntervalSeconds"];
	if (!sweep_str.empty()) {
		int parsed = atoi(sweep_str.c_str());
		if (parsed > 0) {
			return parsed;
		}
	}
	return 60;
}
} // namespace

CServer::CServer(boost::asio::io_context& io_context, short port, std::shared_ptr<AsioIOServicePool> pool):_io_context(io_context), _port(port),
_acceptor(io_context, tcp::endpoint(tcp::v4(),port)), _timer(_io_context, std::chrono::seconds(HeartbeatSweepSeconds())), _pool(pool)
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
			//登记完成后才启动读取，避免极速断连先 Close、随后又被插回连接表。
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

int CServer::GetAuthenticatedSessionCount() {
	std::map<std::string, shared_ptr<CSession>> sessions_copy;
	{
		lock_guard<mutex> lock(_mutex);
		sessions_copy = _sessions;
	}
	int count = 0;
	for (const auto& kv : sessions_copy) {
		if (kv.second && kv.second->GetUserId() > 0) {
			count++;
		}
	}
	return count;
}

void CServer::on_timer(const boost::system::error_code& ec) {
	if (ec) {
		if (ec != boost::asio::error::operation_aborted) {
			std::cout << "timer error: " << ec.message() << std::endl;
		}
		return;
	}
	//此处加锁遍历session
	std::map<std::string, shared_ptr<CSession>> sessions_copy;
	{
		lock_guard<mutex> lock(_mutex);
		sessions_copy = _sessions;
	}

	time_t now = std::time(nullptr);
	for (auto iter = sessions_copy.begin(); iter != sessions_copy.end(); iter++) {
		auto b_expired = iter->second->IsHeartbeatExpired(now);
		if (b_expired) {
			//所有终止原因只调用会话的幂等关闭入口。
			iter->second->Close();
		}
	}

	_timer.expires_after(std::chrono::seconds(HeartbeatSweepSeconds()));
	_timer.async_wait([this](boost::system::error_code ec) {
		on_timer(ec);
	});
}

void CServer::StartTimer()
{
	//启动定时器
	auto self(shared_from_this());
	_timer.async_wait([self](boost::system::error_code ec) {
		self->on_timer(ec);
		});
}

void CServer::StopTimer()
{
	_timer.cancel();
}

void CServer::Stop()
{
	if (_stopped.exchange(true)) {
		return;
	}

	boost::system::error_code ignored;
	try {
		_timer.cancel();
	}
	catch (const boost::system::system_error&) {
		// Stop is also called from the destructor and must not throw.
	}
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
