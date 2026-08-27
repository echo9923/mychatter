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
	/// 资源上传完成点：单事务 UPDATE chat_message.resource_status=1（0→1 条件更新，
	/// 上传完成时原子设置 Ready 并为接收者分配 recv_seq；重复完成返回原序号。
	bool CompleteResourceUpload(long long chat_message_id, int sender_id, int recv_id,
		unsigned long long& recv_seq);
	std::shared_ptr<ChatMessage> GetChatMsgById(long long message_id);
	/// 清理任务：捞取 updated_at 早于 before_time 且 resource_status=0 的资源消息
	bool GetExpiredResourceIds(const std::string& before_time, int limit,
		std::vector<ExpiredResource>& out);
	/// 清理任务：事务内 resource_status 0→2；未发布资源不分配 recv_seq。
	bool MarkResourceExpired(const std::vector<ExpiredResource>& items);

private:
	std::unique_ptr<MySqlPool> pool_;
};
