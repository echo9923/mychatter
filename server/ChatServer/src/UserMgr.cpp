#include "UserMgr.h"
#include "CSession.h"
#include "RedisMgr.h"

UserMgr:: ~ UserMgr(){
	_uid_to_session.clear();
}


std::shared_ptr<CSession> UserMgr::GetSession(int uid)
{
	std::lock_guard<std::mutex> lock(_session_mtx);
	auto iter = _uid_to_session.find(uid);
	if (iter == _uid_to_session.end()) {
		return nullptr;
	}

	return iter->second;
}

void UserMgr::SetUserSession(int uid, std::shared_ptr<CSession> session)
{
	std::lock_guard<std::mutex> lock(_session_mtx);
	_uid_to_session[uid] = session;
}

void UserMgr::RmvUserSession(int uid, const std::shared_ptr<CSession>& session)
{ 
	{
		std::lock_guard<std::mutex> lock(_session_mtx);
		auto iter = _uid_to_session.find(uid);
		if (iter == _uid_to_session.end()) {
			return;
		}
	
		//不相等说明旧会话的延迟清理遇到了新的登录会话
		if (iter->second != session) {
			return;
		}
		_uid_to_session.erase(uid);
	}

}

UserMgr::UserMgr()
{

}
