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
	bool UpdateUploadStatus(int chat_messag_id);
	std::shared_ptr<ChatMessage> GetChatMsgById(int message_id);
private:
	MysqlMgr();
	MysqlDao  _dao;
};
