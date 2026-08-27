#include "MysqlMgr.h"


MysqlMgr::~MysqlMgr() {

}

int MysqlMgr::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	return _dao.RegUser(name, email, pwd);
}

bool MysqlMgr::CheckEmail(const std::string& name, const std::string& email) {
	return _dao.CheckEmail(name, email);
}

bool MysqlMgr::UpdatePwd(const std::string& name, const std::string& pwd) {
	return _dao.UpdatePwd(name, pwd);
}

MysqlMgr::MysqlMgr() {
}

bool MysqlMgr::CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo) {
	return _dao.CheckPwd(name, pwd, userInfo);
}

FriendOperationResult MysqlMgr::AddFriendApply(int from, int to, const std::string& desc,
	const std::string& requester_remark, const std::string& unique_id,
	std::shared_ptr<ChatMessage>& application) {
	return _dao.AddFriendApply(from, to, desc, requester_remark, unique_id, application);
}

FriendOperationResult MysqlMgr::HandleFriendApply(int handler_uid,
	std::int64_t apply_message_id, bool accept, const std::string& handler_remark,
	const std::string& reason, FriendHandleOutput& output) {
	return _dao.HandleFriendApply(handler_uid, apply_message_id, accept,
		handler_remark, reason, output);
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(int uid)
{
	return _dao.GetUser(uid);
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(std::string name)
{
	return _dao.GetUser(name);
}

bool MysqlMgr::GetApplyList(int touid,
	std::vector<std::shared_ptr<ApplyInfo>>& applyList,
	std::int64_t after_message_id, int limit) {

	return _dao.GetApplyList(touid, applyList, after_message_id, limit);
}

bool MysqlMgr::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info) {
	return _dao.GetFriendList(self_id, user_info);
}

bool MysqlMgr::GetUserThreads(int64_t userId,
	int64_t lastId,
	int      pageSize,
	std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
	bool& loadMore,
	int64_t& nextLastId)
{
	return _dao.GetUserThreads(userId, lastId, pageSize, threads, loadMore, nextLastId);
}

bool MysqlMgr::CreatePrivateChat(int user1_id, int user2_id, std::int64_t& thread_id)
{
	return _dao.CreatePrivateChat(user1_id, user2_id, thread_id);
}

bool MysqlMgr::GetPrivateChatMembers(std::int64_t thread_id, int& user1, int& user2)
{
	return _dao.GetPrivateChatMembers(thread_id, user1, user2);
}

std::shared_ptr<ChatMessage> MysqlMgr::GetChatMsgById(std::int64_t message_id)
{
	return _dao.GetChatMsgById(message_id);
}

std::shared_ptr<PageResult> MysqlMgr::LoadChatMsg(std::int64_t threadId, std::int64_t lastId, int pageSize)
{
	return _dao.LoadChatMsg(threadId, lastId, pageSize);
}

SaveMessageResult MysqlMgr::AddChatMsg(std::shared_ptr<ChatMessage> chat_data) {
	return _dao.AddChatMsg(chat_data);
}

bool MysqlMgr::GetMessagesAfterRecvSeq(int uid, std::uint64_t after_recv_seq, int limit,
	std::vector<SyncedMessage>& messages) {
	return _dao.GetMessagesAfterRecvSeq(uid, after_recv_seq, limit, messages);
}

bool MysqlMgr::GetLastRecvSeq(int uid, std::uint64_t& last_seq) {
	return _dao.GetLastRecvSeq(uid, last_seq);
}

