#include "CServer.h"
#include <iostream>
#include <ctime>
#include "AsioIOServicePool.h"
#include "UserMgr.h"
#include "RedisMgr.h"

CServer::CServer(boost::asio::io_context& io_context, short port):_io_context(io_context), _port(port),
_acceptor(io_context, tcp::endpoint(tcp::v4(),port)), _timer(_io_context, std::chrono::seconds(60))
{
	cout << "Server start success, listen on port : " << _port << endl;

	StartAccept();
}

CServer::~CServer() {
	cout << "Server destruct listen on port : " << _port << endl;
	
}

void CServer::HandleAccept(shared_ptr<CSession> new_session, const boost::system::error_code& error){
	if (!error) {
		new_session->Start();
		lock_guard<mutex> lock(_mutex);
		_sessions.insert(make_pair(new_session->GetSessionId(), new_session));
	}
	else {
		cout << "session accept failed, error is " << error.what() << endl;
	}

	StartAccept();
}

void CServer::StartAccept() {
	auto &io_context = AsioIOServicePool::GetInstance()->GetIOService();
	shared_ptr<CSession> new_session = make_shared<CSession>(io_context, this);
	_acceptor.async_accept(new_session->GetSocket(), std::bind(&CServer::HandleAccept, this, new_session, placeholders::_1));
}

//根据session 的id删除session，并移除用户和session的关联
void CServer::ClearSession(std::string session_id) {
	
	lock_guard<mutex> lock(_mutex);
	if (_sessions.find(session_id) != _sessions.end()) {
		auto uid = _sessions[session_id]->GetUserId();

		//移除用户和session的关联
		UserMgr::GetInstance()->RmvUserSession(uid, session_id);
	}

	_sessions.erase(session_id);
	
}

//根据用户获取session
shared_ptr<CSession> CServer::GetSession(std::string uuid) {
	lock_guard<mutex> lock(_mutex);
	auto it = _sessions.find(uuid);
	if (it != _sessions.end()) {
		return it->second;
	}
	return nullptr;
}

bool CServer::CheckValid(std::string uuid)
{
	lock_guard<mutex> lock(_mutex);
	auto it = _sessions.find(uuid);
	if (it != _sessions.end()) {
		return true;
	}
	return false;
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
		std::cout << "timer error: " << ec.message() << std::endl;
		return;
	}
	std::vector<std::shared_ptr<CSession>> _expired_sessions;
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
			//关闭socket, 其实这里也会触发async_read的错误处理
			iter->second->Close();
			//收集过期信息
			_expired_sessions.push_back(iter->second);
			continue;
		}
	}

	//处理过期session, 单独提出，防止死锁
	for (auto &session : _expired_sessions) {
		session->DealExceptionSession();
	}

	//可恢复令牌续期：每约 3600s（60 个 tick）对在线会话做一次 compare-and-expire。
	//仅当 Redis 中令牌仍与本会话一致才刷新 TTL；异地重新登录会轮换令牌使旧连接不续期、尽快失效。
	if (++_token_refresh_tick >= 60) {
		_token_refresh_tick = 0;
		for (auto iter = sessions_copy.begin(); iter != sessions_copy.end(); iter++) {
			auto& s = iter->second;
			if (s && s->GetUserId() > 0) {
				RedisMgr::GetInstance()->CompareAndExpire(
					SESSION_TOKEN_V2_PREFIX + std::to_string(s->GetUserId()),
					s->GetSessionToken(), 86400);
			}
		}
	}
	
	//再次设置，下一个60s检测
	_timer.expires_after(std::chrono::seconds(60));
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
