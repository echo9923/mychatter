#pragma once
#include "const.h"
#include "MysqlDao.h"
#include "Singleton.h"
#include <vector>

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
	FriendOperationResult AddFriendApply(int from, int to, const std::string& desc,
		const std::string& requester_remark, const std::string& unique_id,
		std::shared_ptr<ChatMessage>& application);
	FriendOperationResult HandleFriendApply(int handler_uid, std::int64_t apply_message_id,
		bool accept, const std::string& handler_remark, const std::string& reason,
		FriendHandleOutput& output);
	/// 根据uid获取用户信息
	std::shared_ptr<UserInfo> GetUser(int uid);
	/// 根据用户名获取用户信息
	std::shared_ptr<UserInfo> GetUser(std::string name);
	/// 获取好友申请列表（分页）
	bool GetApplyList(int touid, std::vector<std::shared_ptr<ApplyInfo>>& applyList,
		std::int64_t after_message_id, int limit = 100);
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
	/// 取私聊会话两成员（1503 资源消息会话归属校验用）；会话不存在返回 false
	bool GetPrivateChatMembers(std::int64_t thread_id, int& user1, int& user2);
	/// 分页加载历史聊天消息
	std::shared_ptr<PageResult> LoadChatMsg(std::int64_t threadId, std::int64_t lastId, int pageSize);
	/// 插入单条聊天消息（幂等），返回持久化结果；成功/重复时回写 canonical message_id
	SaveMessageResult AddChatMsg(std::shared_ptr<ChatMessage> chat_data);
	/// 拉取用户在指定接收序号之后的消息（按 recv_seq 升序，多取一条供 has_more）
	bool GetMessagesAfterRecvSeq(int uid, std::uint64_t after_recv_seq, int limit,
		std::vector<SyncedMessage>& messages);
	/// 取用户当前接收序号头
	bool GetLastRecvSeq(int uid, std::uint64_t& last_seq);
	/// 按 message_id 取单条消息（服务间 gRPC 通知回读用）；不存在返回 nullptr
	std::shared_ptr<ChatMessage> GetChatMsgById(std::int64_t message_id);

private:
	/// 私有构造函数，初始化MysqlDao
	MysqlMgr();
	/// 数据访问对象实例，实际执行SQL操作
	MysqlDao  _dao;
};

