#pragma once
#include "const.h"
#include "MysqlDao.h"
#include "Singleton.h"
#include <vector>
#include "chat.pb.h"
#include "FileInfo.h"

class MysqlMgr: public Singleton<MysqlMgr>
{
	friend class Singleton<MysqlMgr>;
public:
	~MysqlMgr();
	std::shared_ptr<UserInfo> GetUser(int uid);
	bool UpdateUserIcon(int uid, const std::string& icon);
	/// 资源上传完成点：单事务 resource_status 0→1 并分配接收者 recv_seq。
	bool CompleteResourceUpload(long long chat_message_id, int sender_id, int recv_id,
		unsigned long long& recv_seq);
	std::shared_ptr<ChatMessage> GetChatMsgById(long long message_id);
	/// 清理任务：捞取超时未完成的资源消息
	bool GetExpiredResourceIds(const std::string& before_time, int limit,
		std::vector<ExpiredResource>& out);
	/// 清理任务：resource_status 0→2；未发布资源不进入接收者消息流。
	bool MarkResourceExpired(const std::vector<ExpiredResource>& items);
private:
	MysqlMgr();
	MysqlDao  _dao;
};
