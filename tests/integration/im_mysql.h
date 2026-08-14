// im_mysql.h — mysql-concpp wrapper for chat_message / user_message_sync
// verification and teardown (plan Verification.6).
// Single short-lived connection per scenario.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Include the mysql-concpp JDBC headers so sql::Connection is a complete type.
// Forward-declaring `struct sql::Connection` would not work (sql is a
// namespace, not a class); and an elaborated forward declaration of
// sql::Connection without these headers leaves the type incomplete.
#include <jdbc/mysql_connection.h>
#include <jdbc/cppconn/connection.h>

namespace imt {

struct ChatMessageRow {
	std::int64_t message_id = 0;   // chat_message.message_id 为 bigint UNSIGNED
	std::int64_t thread_id  = 0;
	int         sender_id  = 0;
	int         recv_id    = 0;
	std::string unique_id;
	std::string content;
	std::string chat_time;
	int         status          = 0;   // MsgStatus 纯阅读态 (UN_READ=0 ...)
	int         msg_type        = 0;   // ChatMsgType (TEXT=0, PIC=1, FILE=3 ...)
	int         resource_status = 0;   // 0=Uploading 1=Ready 2=Expired
	std::string content_hash;          // 整文件 SHA-256（资源消息）
	std::uint64_t content_size  = 0;
	int         delivery_status = 0;   // 0=Pending, 1=Acked
};

// user_message_sync 的一行（增量同步游标表）。
struct SyncRow {
	std::uint64_t sync_seq   = 0;
	std::uint64_t message_id = 0;
};

class Mysql {
public:
	Mysql() = default;
	~Mysql();
	Mysql(const Mysql&) = delete;
	Mysql& operator=(const Mysql&) = delete;

	bool Connect(const std::string& host, int port, const std::string& user,
	             const std::string& pwd, const std::string& schema);
	void Close();
	bool connected() const { return con_ != nullptr; }

	// All rows for a given (sender_id, unique_id).
	std::vector<ChatMessageRow> QueryByUniqueId(int sender_id, const std::string& unique_id);

	// COUNT(*) for exact unique_id (across senders).
	long long CountByUniqueId(const std::string& unique_id);

	// COUNT(*) for unique_id matching a LIKE pattern (e.g. "imtest-%").
	long long CountByUniqueIdLike(const std::string& pattern);

	// COUNT(*) for unique_id LIKE pattern AND a specific delivery_status.
	// delivery_status: 0=Pending, 1=Acked.
	long long CountByUniqueIdLikeAndDelivery(const std::string& pattern,
	                                         int delivery_status);

	// All rows for a specific message_id (single-row expected).
	std::vector<ChatMessageRow> QueryByMessageId(std::int64_t message_id);

	// All chat_message rows for recv_id with a given delivery_status.
	std::vector<ChatMessageRow> QueryByRecvIdAndDelivery(int recv_id,
	                                                    int delivery_status);

	// Delete every chat_message row whose unique_id matches the LIKE pattern.
	// Returns rows affected, or -1 on error.
	long long DeleteByUniqueIdLike(const std::string& pattern);

	// ---- user_message_sync 断言辅助（增量同步） ------------------------------

	// 某 uid 在 after_seq 之后的 sync 行，按 sync_seq 升序；limit<=0 不限。
	std::vector<SyncRow> QuerySyncRows(std::int64_t uid, std::uint64_t after_seq,
	                                   int limit);

	// 某 uid 当前最大 sync_seq（无行返回 0，对应 bootstrap checkpoint）。
	std::uint64_t MaxSyncSeq(std::int64_t uid);

	// COUNT(*) of sync rows whose message belongs to unique_id LIKE pattern.
	long long CountSyncRowsByUniqueIdLike(const std::string& pattern);

	// 删除 unique_id 匹配 LIKE 模式的消息对应的 sync 行（需先于
	// DeleteByUniqueIdLike 调用，否则 JOIN 不到）。返回受影响行数，-1 出错。
	long long DeleteSyncRowsByUniqueIdLike(const std::string& pattern);

	// chat_message 当前 AUTO_INCREMENT（big-ids 场景结束后恢复用），-1 出错。
	long long GetChatMessageAutoIncrement();

	// ALTER TABLE chat_message AUTO_INCREMENT = value。
	bool SetChatMessageAutoIncrement(std::int64_t value);

	// 将指定消息的 updated_at 回拨到 days_ago 天前（resource-expiry 场景伪造
	// 超时未完成的资源行，供 ResourceServer 清理任务捞取）。
	bool BackdateMessageUpdatedAt(std::int64_t message_id, int days_ago);

private:
	sql::Connection* con_ = nullptr;  // owned, freed in Close()
};

} // namespace imt
