#pragma once
#include "const.h"
#include <thread>
#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>
#include "data.h"
#include <memory>
#include <queue>
#include <mutex>
#include "FileInfo.h"

#include "MySqlPool.h"

class MysqlDao
{
public:
	MysqlDao();
	~MysqlDao();
	std::shared_ptr<UserInfo> GetUser(int uid);
	bool UpdateHeadInfo(int uid, const std::string& icon);
	/// 资源上传完成点：发布消息并在同一事务中为接收者分配 user_events.event_seq。
	/// 重复完成返回原 event_seq，不重复创建事件。
	bool CompleteResourceUpload(long long message_id, int sender_user_id,
		unsigned long long& event_seq);
	std::shared_ptr<ChatMessage> GetChatMsgById(long long message_id);
	/// 清理任务：捞取 created_at 早于 before_time 且 status=PENDING 的资源消息。
	bool GetExpiredResourceIds(const std::string& before_time, int limit,
		std::vector<ExpiredResource>& out);
	/// 清理任务：事务内 status PENDING→FAILED；未发布资源不创建用户事件。
	bool MarkResourceExpired(const std::vector<ExpiredResource>& items);

private:
	std::unique_ptr<MySqlPool> pool_;
};
