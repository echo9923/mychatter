#pragma once
#include "const.h"
#include "MysqlDao.h"
#include "Singleton.h"
#include <vector>
#include "chat.pb.h"

/**
 * @brief MySQL管理器（单例）
 * 
 * 作为MysqlDao的上层封装，提供全局唯一的数据库访问入口。
 * 业务层通过 MysqlMgr::GetInstance() 获取单例后调用数据库操作，
 * 内部委托给MysqlDao完成实际的SQL执行。
 */
class MysqlMgr: public Singleton<MysqlMgr>
{
	friend class Singleton<MysqlMgr>;
public:
	/// 析构函数
	~MysqlMgr();

	/// 注册新用户，返回新uid，失败返回-1
	int RegUser(const std::string& name, const std::string& email,  const std::string& pwd);
	/// 检查用户名和邮箱是否匹配
	bool CheckEmail(const std::string& name, const std::string & email);
	/// 更新用户密码
	bool UpdatePwd(const std::string& name, const std::string& email);
	/// 验证用户名密码，成功则填充userInfo
	bool CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo);
	/// 添加好友申请记录
	bool AddFriendApply(const int& from, const int& to, const std::string& desc, const std::string& back_name);
	/// 添加好友关系（双向插入）
	bool AddFriend(const int& from, const int& to, std::string back_name, std::vector<std::shared_ptr<AddFriendMsg>>& msg_list);
	/// 根据uid获取用户信息
	std::shared_ptr<UserInfo> GetUser(int uid);
	/// 根据用户名获取用户信息
	std::shared_ptr<UserInfo> GetUser(std::string name);
	/// 获取好友申请列表（分页）
	bool GetApplyList(int touid, std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit=10);
	/// 获取好友列表
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info);
	/// 分页查询用户聊天会话列表
	bool GetUserThreads(int64_t userId,
		int64_t lastId,
		int      pageSize,
		std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
		bool& loadMore,
		int64_t& nextLastId);
	/// 创建私聊会话
	bool CreatePrivateChat(int user1_id, int user2_id, std::int64_t &thread_id);
	/// 分页加载历史聊天消息
	std::shared_ptr<PageResult> LoadChatMsg(std::int64_t threadId, std::int64_t lastId, int pageSize);
	/// 插入单条聊天消息（幂等），返回持久化结果；成功/重复时回写 canonical message_id
	SaveMessageResult AddChatMsg(std::shared_ptr<ChatMessage> chat_data);
	/// 拉取用户在指定同步序号之后的消息（增量同步，按 sync_seq 升序，多取一条供 has_more）
	bool GetMessagesAfterSyncSeq(int uid, std::uint64_t after_sync_seq, int limit,
		std::vector<SyncedMessage>& messages);
	/// 取用户当前最大同步序号（bootstrap checkpoint）
	bool GetMaxSyncSeq(int uid, std::uint64_t& max_seq);
	/// 按 recv_uid+ids 批量取回消息（防越权）
	std::vector<std::shared_ptr<ChatMessage>> GetMessagesByIds(int recv_uid,
		const std::vector<std::int64_t>& ids);
	/// 将指定接收者的一批消息标记为已投递（ACK，带 recv_id 防越权，幂等）
	bool MarkMessagesDelivered(int recv_uid, const std::vector<std::int64_t>& ids);

private:
	/// 私有构造函数，初始化MysqlDao
	MysqlMgr();
	/// 数据访问对象实例，实际执行SQL操作
	MysqlDao  _dao;
};

