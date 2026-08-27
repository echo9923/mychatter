#pragma once
#include "const.h"
#include <thread>
#include <jdbc/mysql_driver.h>
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/prepared_statement.h>
#include <jdbc/cppconn/resultset.h>
#include <jdbc/cppconn/statement.h>
#include <jdbc/cppconn/exception.h>
#include <jdbc/cppconn/datatype.h>
#include "data.h"
#include <memory>
#include <queue>
#include <mutex>
#include "MySqlPool.h"

/**
 * @brief 聊天消息持久化结果枚举
 *
 * 表示一次 AddChatMsg 写入的幂等结果，供上层决定是否向 sender 返回成功/冲突/失败。
 * - Stored:   新行写入成功
 * - Duplicate:同 (sender_id, unique_id) 已存在且内容完全一致，返回同一 message_id，不重复插入
 * - Conflict: 同 (sender_id, unique_id) 已存在但内容不一致，原行不变（永久冲突）
 * - Failed:   SQL 错误或连接失败，本批新行已回滚
 */
enum class SaveMessageResult {
	Stored,    ///< 新消息已持久化
	Duplicate, ///< 幂等重复，返回已有 canonical message_id
	Conflict,  ///< unique-id 相同但内容冲突，原消息不变
	Failed     ///< 持久化失败（SQL 错误/连接失败，已回滚）
};

enum class FriendOperationResult {
	Stored,
	Duplicate,
	Conflict,
	NotFound,
	AlreadyHandled,
	AlreadyFriends,
	Forbidden,
	Failed
};

struct FriendHandleOutput {
	std::shared_ptr<ChatMessage> application;
	std::shared_ptr<ChatMessage> result_message;
	std::int64_t thread_id{0};
	int peer_uid{0};
	std::string handler_contact_remark;
};

/**
 * @brief MySQL数据访问对象(DAO)
 * 
 * 封装所有数据库操作，包括用户管理、好友关系、聊天会话、聊天消息等。
 * 内部使用MySqlPool连接池管理数据库连接，每次操作从池中获取连接，
 * 使用完毕后归还。
 */
class MysqlDao
{
public:
	/// 构造函数，初始化连接池
	MysqlDao();
	/// 析构函数，关闭连接池
	~MysqlDao();

	/**
	 * @brief 注册新用户
	 * @param name 用户名
	 * @param email 邮箱
	 * @param pwd 密码
	 * @return 新用户的uid，失败返回-1
	 */
	int RegUser(const std::string& name, const std::string& email, const std::string& pwd);

	/**
	 * @brief 检查用户名和邮箱是否匹配（用于重置密码验证）
	 * @param name 用户名
	 * @param email 邮箱
	 * @return 是否匹配
	 */
	bool CheckEmail(const std::string& name, const std::string & email);

	/**
	 * @brief 更新用户密码
	 * @param name 用户名
	 * @param newpwd 新密码
	 * @return 是否更新成功
	 */
	bool UpdatePwd(const std::string& name, const std::string& newpwd);

	/**
	 * @brief 验证用户名和密码，成功则填充用户信息
	 * @param name 用户名
	 * @param pwd 密码
	 * @param userInfo [out] 验证成功时填充的用户信息
	 * @return 是否验证成功
	 */
	bool CheckPwd(const std::string& name, const std::string& pwd, UserInfo& userInfo);

	/**
	 * @brief 添加好友申请记录到数据库
	 * @param from 申请者uid
	 * @param to 目标用户uid
	 * @param desc 申请附言
	 * @param back_name 申请者昵称/备注名
	 * @return 是否成功
	 */
	FriendOperationResult AddFriendApply(int from, int to, const std::string& desc,
		const std::string& requester_remark, const std::string& unique_id,
		std::shared_ptr<ChatMessage>& application);

	/**
	 * @brief 添加好友关系（双向插入好友表，并生成聊天数据）
	 * @param from 用户A uid
	 * @param to 用户B uid
	 * @param back_name 备注名
	 * @param chat_datas [out] 生成的聊天数据（用于通知客户端）
	 * @return 是否成功
	 */
	FriendOperationResult HandleFriendApply(int handler_uid, std::int64_t apply_message_id,
		bool accept, const std::string& handler_remark, const std::string& reason,
		FriendHandleOutput& output);
	
	/**
	 * @brief 根据uid获取用户信息
	 * @param uid 用户ID
	 * @return 用户信息智能指针，不存在则返回nullptr
	 */
	std::shared_ptr<UserInfo> GetUser(int uid);

	/**
	 * @brief 根据用户名获取用户信息
	 * @param name 用户名
	 * @return 用户信息智能指针，不存在则返回nullptr
	 */
	std::shared_ptr<UserInfo> GetUser(std::string name);

	/**
	 * @brief 获取指定用户的好友申请列表（分页）
	 * @param touid 目标用户uid
	 * @param applyList [out] 输出的申请列表
	 * @param offset 分页偏移量
	 * @param limit 每页数量
	 * @return 是否成功
	 */
	bool GetApplyList(int touid, std::vector<std::shared_ptr<ApplyInfo>>& applyList,
		std::int64_t after_message_id, int limit);

	/**
	 * @brief 获取指定用户的好友列表
	 * @param self_id 用户ID
	 * @param user_info [out] 输出的好友信息列表
	 * @return 是否成功
	 */
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info);

	/**
	 * @brief 分页查询用户的聊天会话列表
	 * @param userId 用户ID
	 * @param lastId 游标（上一页最后一条ID）
	 * @param pageSize 每页数量
	 * @param threads [out] 输出的会话列表
	 * @param loadMore [out] 是否还有更多
	 * @param nextLastId [out] 下一页游标
	 * @return 是否成功
	 */
	bool GetUserThreads(
		int64_t userId,
		int64_t lastId,
		int      pageSize,
		std::vector<std::shared_ptr<ChatThreadInfo>>& threads,
		bool& loadMore,
		int64_t& nextLastId);

	/**
	 * @brief 创建私聊会话（若已存在则返回已有会话ID）
	 * @param user1_id 用户A ID
	 * @param user2_id 用户B ID
	 * @param thread_id [out] 输出的会话ID
	 * @return 是否成功
	 */
	bool CreatePrivateChat(int user1_id, int user2_id, std::int64_t& thread_id);

	/**
	 * @brief 取私聊会话的两个成员（1503 资源消息创建的会话归属校验用）
	 *
	 * @param thread_id 会话ID
	 * @param user1 [out] 成员一（表中存储顺序，非大小序）
	 * @param user2 [out] 成员二
	 * @return 会话存在返回 true；不存在/查询失败返回 false
	 */
	bool GetPrivateChatMembers(std::int64_t thread_id, int& user1, int& user2);

	/**
	 * @brief 分页加载指定会话的历史聊天消息
	 * @param threadId 会话ID
	 * @param lastId 游标（上一页最后一条消息ID）
	 * @param pageSize 每页数量
	 * @return 分页结果智能指针
	 */
	std::shared_ptr<PageResult> LoadChatMsg(std::int64_t threadId, std::int64_t lastId, int pageSize);

	/**
	 * @brief 插入单条聊天消息到数据库（幂等）
	 * @param chat_data 消息智能指针；成功/重复时会回写 canonical message_id
	 * @return 持久化结果
	 */
	SaveMessageResult AddChatMsg(std::shared_ptr<ChatMessage> chat_data);

	/**
	 * @brief 拉取用户在指定同步序号之后的消息（增量同步）
	 *
	 * 直接按 chat_message.recv_seq 严格升序返回，实际多取一条
	 * （limit+1）供调用方判断 has_more。
	 *
	 * @param uid 用户ID
	 * @param after_recv_seq 接收游标（仅返回 recv_seq 大于该值的消息，0 表示从头）
	 * @param limit 期望条数上限（实际最多返回 limit+1 条）
	 * @param messages [out] 输出的同步消息列表（升序）
	 * @return 是否执行成功（false 时调用方应回错误，不得当作空页）
	 */
	bool GetMessagesAfterRecvSeq(int uid, std::uint64_t after_recv_seq, int limit,
		std::vector<SyncedMessage>& messages);

	/**
	 * @brief 取用户当前接收序号头
	 * @param uid 用户ID
	 * @param last_seq [out] user.last_recv_seq
	 * @return 是否执行成功
	 */
	bool GetLastRecvSeq(int uid, std::uint64_t& last_seq);

	/**
	 * @brief 按 message_id 取单条消息（gRPC 资源通知回读 DB 组 envelope 用）
	 *
	 * 不带 uid 过滤：调用方为服务间 gRPC 通知入口，不接受客户端直连请求。
	 *
	 * @param message_id 消息ID
	 * @return 消息；不存在/查询失败返回 nullptr
	 */
	std::shared_ptr<ChatMessage> GetChatMsgById(std::int64_t message_id);

private:
	/// MySQL连接池实例，管理数据库连接的复用和保活
	std::unique_ptr<MySqlPool> pool_;

	/**
	 * @brief 单条消息幂等 UPSERT + 回读核对（内部复用）
	 *
	 * 执行 INSERT ... ON DUPLICATE KEY UPDATE message_id=LAST_INSERT_ID(message_id)，
	 * 随后按 canonical message_id 回读并核对 thread_id/recv_id/content/msg_type/content_size：
	 * 完全一致返回 Duplicate（同一 message_id），不一致返回 Conflict（不覆盖原消息）。
	 * 新的可见消息在同一事务中锁定接收者 user 行、分配 recv_seq；资源元数据
	 * 暂不分配，待 ResourceServer 校验完成时发布。Duplicate/Conflict 回滚临时序号。
	 *
	 * @param conn 已处于事务中的连接（调用方管理 commit/rollback）
	 * @param msg 待写入消息；成功/重复时回写 canonical message_id
	 * @param out_conflict_uid [out] 冲突时写入该消息的 unique_id
	 * @return Stored/Duplicate/Conflict/Failed
	 */
	SaveMessageResult UpsertChatMessage(sql::Connection* conn,
		const std::shared_ptr<ChatMessage>& msg,
		std::string& out_conflict_uid);
	bool AllocateRecvSeq(sql::Connection* conn, int uid, std::uint64_t& recv_seq);
	std::shared_ptr<ChatMessage> ReadMessage(sql::ResultSet* rs);
};


