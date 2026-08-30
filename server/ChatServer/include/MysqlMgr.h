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
	FriendOperationResult AddFriendApply(int requester_user_id, int target_user_id,
		const std::string& request_message, const std::string& client_request_id,
		std::shared_ptr<FriendRequest>& request);
	FriendOperationResult HandleFriendApply(int handler_user_id,
		std::int64_t friend_request_id, bool accept, FriendHandleOutput& output);
	/// 根据uid获取用户信息
	std::shared_ptr<UserInfo> GetUser(int uid);
	/// 根据用户名获取用户信息
	std::shared_ptr<UserInfo> GetUser(std::string name);
	/// 获取好友申请列表（分页）
	bool GetApplyList(int target_user_id, std::vector<std::shared_ptr<ApplyInfo>>& apply_list,
		std::int64_t after_friend_request_id, int limit = 100);
	/// 获取好友列表
	bool GetFriendList(int user_id, std::vector<ContactInfo>& contacts);
	/// 分页查询用户聊天会话列表
	bool GetUserThreads(int64_t userId,
		int64_t lastId,
		int      pageSize,
		std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
		bool& loadMore,
		int64_t& nextLastId);
	/// 创建私聊会话
	bool CreatePrivateChat(int requester_user_id, int target_user_id, std::int64_t &thread_id);
	/// 取私聊会话两成员（1503 资源消息会话归属校验用）；会话不存在返回 false
	bool GetPrivateChatMembers(std::int64_t thread_id, int& lower_user_id, int& higher_user_id);
	/// 分页加载历史聊天消息
	std::shared_ptr<PageResult> LoadChatMsg(int requester_user_id,
		std::int64_t thread_id, std::int64_t before_message_id, int page_size);
	/// 插入单条聊天消息（幂等），返回持久化结果；成功/重复时回写 canonical message_id
	SaveMessageResult AddChatMsg(std::shared_ptr<ChatMessage> chat_data);
	/// 拉取用户在指定事件序号之后的事件（按 event_seq 升序，多取一条供 has_more）
	bool GetEventsAfterSeq(int user_id, std::uint64_t after_event_seq, int limit,
		std::vector<UserEvent>& events);
	bool GetUserEvent(int recipient_user_id, int event_type, std::int64_t message_id,
		std::int64_t friend_request_id, UserEvent& event);
	bool GetLastEventSeq(int user_id, std::uint64_t& last_event_seq);
	/// 按 message_id 取单条消息（服务间 gRPC 通知回读用）；不存在返回 nullptr
	std::shared_ptr<ChatMessage> GetChatMsgById(std::int64_t message_id);
	std::shared_ptr<FriendRequest> GetFriendRequestById(std::int64_t friend_request_id);

private:
	/// 私有构造函数，初始化MysqlDao
	MysqlMgr();
	/// 数据访问对象实例，实际执行SQL操作
	MysqlDao  _dao;
};

