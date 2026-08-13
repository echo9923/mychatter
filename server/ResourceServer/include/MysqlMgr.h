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
	/// 图片上传完成点：单事务状态迁移 + 双方同步行（见 MysqlDao）
	bool UpdateUploadStatusWithSync(long long chat_messag_id, int sender_id, int recv_id);
	std::shared_ptr<ChatMessage> GetChatMsgById(long long message_id);
private:
	MysqlMgr();
	MysqlDao  _dao;
};
