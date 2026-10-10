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
	/// 发布事务结果（调用方据此映射 1506 错误码）：
	/// Published=本次发布成功；AlreadyPublished=此前已发布（幂等重试），
	/// event_seq 带回原值；NotPending=状态已是 Failed 等终态（permanent，
	/// 映射 2116）；NotFound=消息不存在或 sender 不符（映射 2111）；
	/// Transient=死锁重试耗尽/连接异常（映射 2002，可重试）。
	enum class PublishResult { Published, AlreadyPublished, NotPending, NotFound, Transient };

	MysqlDao();
	~MysqlDao();
	std::shared_ptr<UserInfo> GetUser(int uid);
	bool UpdateHeadInfo(int uid, const std::string& icon);
	/// 资源上传完成点：发布消息并在同一事务中为接收者分配 user_events.event_seq。
	/// 重复完成返回 AlreadyPublished 与原 event_seq，不重复创建事件。
	/// InnoDB 1213 死锁（SQLState 40001）在内部有界重试，耗尽返回 Transient。
	PublishResult CompleteResourceUpload(long long message_id, int sender_user_id,
		unsigned long long& event_seq);
	/// 条件置 Failed：status=PENDING → FAILED，未发布资源不创建用户事件。
	/// 用于整文件哈希连续不符的第 3 次终态化。
	bool MarkResourceFailed(long long message_id);
	/// 单行查询的可区分结果：NotFound 才允许映射为永久错误（2111），
	/// Transient（连接耗尽/SQL 异常）必须按可重试处理（2002），否则会把
	/// 瞬时 DB 故障误标为消息不存在并触发清理误删。
	enum class ChatMsgLookup { Found, NotFound, Transient };
	ChatMsgLookup GetChatMsgById(long long message_id, std::shared_ptr<ChatMessage>& out);
	/// 清理任务：捞取 created_at 早于 before_time 且 status=PENDING 的资源消息，
	/// 按 (created_at, message_id) 游标分页（修复 LIMIT 常取同批导致的饿死）。
	/// after_created_at 为空串时首轮查询：完全省略游标条件（不依赖空串与
	/// DATETIME 的隐式比较）；非空时使用精确 (created_at, message_id) 条件。
	/// 每行带回 created_at 供调用方推进游标（无论是否过期）。
	bool GetExpiredResourceIds(const std::string& before_time,
		const std::string& after_created_at, long long after_message_id,
		int limit, std::vector<ExpiredResource>& out);
	/// 清理任务：事务内 status PENDING→FAILED；未发布资源不创建用户事件。
	bool MarkResourceExpired(const std::vector<ExpiredResource>& items);

private:
	/// 单次发布事务尝试；死锁（1213/40001）时 deadlock 置真由外层重试
	PublishResult CompleteResourceUploadOnce(long long message_id, int sender_user_id,
		unsigned long long& event_seq, bool& deadlock);
	std::unique_ptr<MySqlPool> pool_;
};
