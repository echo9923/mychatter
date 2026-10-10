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
	/// 资源上传完成点：单事务 status PENDING→PUBLISHED 并分配 event_seq。
	/// 结果枚举语义见 MysqlDao::PublishResult（死锁在 DAO 内有界重试）。
	MysqlDao::PublishResult CompleteResourceUpload(long long message_id,
		int sender_user_id, unsigned long long& event_seq);
	/// 条件置 Failed（PENDING→FAILED），整文件哈希连续不符第 3 次终态化用。
	bool MarkResourceFailed(long long message_id);
	/// 单行查询（可区分 NotFound/Transient）：Transient 必须按可重试处理。
	MysqlDao::ChatMsgLookup GetChatMsgById(long long message_id,
		std::shared_ptr<ChatMessage>& out);
	/// 清理任务：按 (created_at, message_id) 游标捞取超时未完成的资源消息。
	bool GetExpiredResourceIds(const std::string& before_time,
		const std::string& after_created_at, long long after_message_id,
		int limit, std::vector<ExpiredResource>& out);
	/// 清理任务：status PENDING→FAILED；未发布资源不进入接收者事件流。
	bool MarkResourceExpired(const std::vector<ExpiredResource>& items);
private:
	MysqlMgr();
	MysqlDao  _dao;
};
