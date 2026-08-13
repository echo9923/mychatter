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
#include "chat.pb.h"
#include "FileInfo.h"
using message::AddFriendMsg;
using message::TextChatData;

#include "MySqlPool.h"

class MysqlDao
{
public:
	MysqlDao();
	~MysqlDao();
	std::shared_ptr<UserInfo> GetUser(int uid);
	bool UpdateHeadInfo(int uid, const std::string& icon);
	/// 图片上传完成点：单事务 UPDATE chat_message.status=2 + INSERT IGNORE user_message_sync 双方同步行
	bool UpdateUploadStatusWithSync(long long chat_message_id, int sender_id, int recv_id);
	std::shared_ptr<ChatMessage> GetChatMsgById(long long message_id);

private:
	std::unique_ptr<MySqlPool> pool_;
};

