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
	int RegUser(const std::string& name, const std::string& email, const std::string& pwd);
	bool CheckEmail(const std::string& name, const std::string & email);
	bool UpdatePwd(const std::string& name, const std::string& newpwd);
	bool CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo);
	bool AddFriendApply(const int& from, const int& to, const std::string& desc, const std::string& back_name);
	bool AuthFriendApply(const int& from, const int& to);
	bool AddFriend(const int& from, const int& to, std::string back_name, std::vector<std::shared_ptr<AddFriendMsg>> &chat_datas);
	std::shared_ptr<UserInfo> GetUser(int uid);
	std::shared_ptr<UserInfo> GetUser(std::string name);
	bool GetApplyList(int touid, std::vector<std::shared_ptr<ApplyInfo>>& applyList, int offset, int limit );
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info);
	bool GetUserThreads(
		int64_t userId,
		int64_t lastId,
		int      pageSize,
		std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
		bool& loadMore,
		int& nextLastId);
	bool CreatePrivateChat(int user1_id, int user2_id, int& thread_id);
	std::shared_ptr<PageResult> LoadChatMsg(int threadId, int lastId, int pageSize);
	bool AddChatMsg(std::vector<std::shared_ptr<ChatMessage>>& chat_datas);
	bool UpdateHeadInfo(int uid, const std::string& icon);
	bool UpdateUploadStatus(int chat_message_id);
	std::shared_ptr<ChatImgInfo> GetImgInfoByMsgId(int message_id);
	std::shared_ptr<ChatMessage> GetChatMsgById(int message_id);

private:
	std::unique_ptr<MySqlPool> pool_;
};


