// im_mysql.h — mysql-concpp wrapper for chat_message verification and teardown
// (plan Verification.6). Single short-lived connection per scenario.
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
	int         message_id = 0;
	int         thread_id  = 0;
	int         sender_id  = 0;
	int         recv_id    = 0;
	std::string unique_id;
	std::string content;
	std::string chat_time;
	int         status          = 0;   // MsgStatus (UN_READ=0 ...)
	int         msg_type        = 0;   // ChatMsgType (TEXT=0, PIC=1 ...)
	std::uint64_t content_size  = 0;
	int         delivery_status = 0;   // 0=Pending, 1=Acked
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
	std::vector<ChatMessageRow> QueryByMessageId(int message_id);

	// All chat_message rows for recv_id with a given delivery_status.
	std::vector<ChatMessageRow> QueryByRecvIdAndDelivery(int recv_id,
	                                                    int delivery_status);

	// Delete every chat_message row whose unique_id matches the LIKE pattern.
	// Returns rows affected, or -1 on error.
	long long DeleteByUniqueIdLike(const std::string& pattern);

private:
	sql::Connection* con_ = nullptr;  // owned, freed in Close()
};

} // namespace imt
