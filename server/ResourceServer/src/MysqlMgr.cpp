#include "MysqlMgr.h"


MysqlMgr::~MysqlMgr() {

}

MysqlMgr::MysqlMgr() {
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(int uid)
{
	return _dao.GetUser(uid);
}

bool MysqlMgr::UpdateUserIcon(int uid, const std::string& icon) {
	return _dao.UpdateHeadInfo(uid, icon);
}

MysqlDao::PublishResult MysqlMgr::CompleteResourceUpload(long long message_id,
	int sender_user_id, unsigned long long& event_seq)
{
	return _dao.CompleteResourceUpload(message_id, sender_user_id, event_seq);
}

bool MysqlMgr::MarkResourceFailed(long long message_id)
{
	return _dao.MarkResourceFailed(message_id);
}

MysqlDao::ChatMsgLookup MysqlMgr::GetChatMsgById(long long message_id,
	std::shared_ptr<ChatMessage>& out)
{
	return _dao.GetChatMsgById(message_id, out);
}

bool MysqlMgr::GetExpiredResourceIds(const std::string& before_time,
	const std::string& after_created_at, long long after_message_id,
	int limit, std::vector<ExpiredResource>& out)
{
	return _dao.GetExpiredResourceIds(before_time, after_created_at,
		after_message_id, limit, out);
}

bool MysqlMgr::MarkResourceExpired(const std::vector<ExpiredResource>& items)
{
	return _dao.MarkResourceExpired(items);
}
