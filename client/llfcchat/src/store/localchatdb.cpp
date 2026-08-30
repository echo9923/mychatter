#include "localchatdb.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include "global.h"

namespace {

const int kSchemaVersion = 5;
const char* kMessageColumns =
	"m.local_message_id,m.message_id,m.client_message_id,m.thread_id,"
	"m.sender_user_id,m.message_type,m.text_content,m.send_status,m.created_at";
const char* kResourceColumns =
	"r.local_message_id,r.original_file_name,r.local_file_path,"
	"r.file_size_bytes,r.sha256,r.mime_type";

QVariant NullableId(qint64 value) {
	return value > 0 ? QVariant::fromValue<qint64>(value) : QVariant(QVariant::LongLong);
}

QVariant NullableText(const QString& value) {
	return value.isEmpty() ? QVariant(QVariant::String) : QVariant(value);
}

QVariant NonNullText(const QString& value) {
	return QVariant(value.isNull() ? QStringLiteral("") : value);
}

} // namespace

LocalChatDb::LocalChatDb(const QString& connectionName)
	: _connection_name(connectionName.isEmpty()
		? QUuid::createUuid().toString() : connectionName) {
}

LocalChatDb::~LocalChatDb() {
	close();
	_db = QSqlDatabase();
	QSqlDatabase::removeDatabase(_connection_name);
}

bool LocalChatDb::open(const QString& dbPath, qint64 selfUserId) {
	_self_user_id = selfUserId;
	_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), _connection_name);
	_db.setDatabaseName(dbPath);
	if (!_db.open()) {
		qWarning() << "[LocalChatDb] open failed:" << _db.lastError().text();
		return false;
	}

	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("PRAGMA journal_mode=WAL"))
		|| !query.exec(QStringLiteral("PRAGMA synchronous=FULL"))
		|| !query.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
		|| !query.exec(QStringLiteral("PRAGMA busy_timeout=5000"))
		|| !query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
		qWarning() << "[LocalChatDb] pragma failed:" << query.lastError().text();
		return false;
	}

	if (query.value(0).toInt() != kSchemaVersion) {
		if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
		const char* tables[] = { "outbox", "message_resources", "messages",
			"conversations", "friend_requests", "contacts", "sync_state" };
		for (const char* table : tables) {
			if (!query.exec(QStringLiteral("DROP TABLE IF EXISTS %1")
				.arg(QString::fromLatin1(table)))) {
				_db.rollback();
				return false;
			}
		}
		if (!_db.commit()) return false;
	}
	if (!initSchema()) return false;
	return query.exec(QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion));
}

void LocalChatDb::close() {
	if (_db.isValid() && _db.isOpen()) _db.close();
}

bool LocalChatDb::isOpen() const {
	return _db.isValid() && _db.isOpen();
}

bool LocalChatDb::initSchema() {
	QSqlQuery query(_db);
	const char* statements[] = {
		"CREATE TABLE IF NOT EXISTS messages("
		"local_message_id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"message_id INTEGER NULL UNIQUE,"
		"client_message_id TEXT NULL UNIQUE,"
		"thread_id INTEGER NOT NULL,"
		"sender_user_id INTEGER NOT NULL,"
		"message_type INTEGER NOT NULL CHECK(message_type IN(0,1,3)),"
		"text_content TEXT NULL,"
		"send_status INTEGER NOT NULL CHECK(send_status IN(0,1,2)),"
		"created_at INTEGER NOT NULL,"
		"CHECK(message_id IS NOT NULL OR client_message_id IS NOT NULL),"
		"CHECK((message_type=0 AND text_content IS NOT NULL) OR "
		"(message_type IN(1,3) AND text_content IS NULL)))",
		"CREATE INDEX IF NOT EXISTS idx_messages_history "
		"ON messages(thread_id,message_id)",
		"CREATE INDEX IF NOT EXISTS idx_messages_local_order "
		"ON messages(thread_id,created_at,local_message_id)",
		"CREATE TABLE IF NOT EXISTS message_resources("
		"local_message_id INTEGER PRIMARY KEY,"
		"original_file_name TEXT NOT NULL,"
		"local_file_path TEXT NULL,"
		"file_size_bytes INTEGER NOT NULL CHECK(file_size_bytes>0),"
		"sha256 TEXT NOT NULL,"
		"mime_type TEXT NOT NULL,"
		"FOREIGN KEY(local_message_id) REFERENCES messages(local_message_id) ON DELETE CASCADE)",
		"CREATE TABLE IF NOT EXISTS conversations("
		"thread_id INTEGER PRIMARY KEY,"
		"peer_user_id INTEGER NOT NULL,"
		"last_message_id INTEGER NULL,"
		"oldest_loaded_message_id INTEGER NULL,"
		"history_complete INTEGER NOT NULL DEFAULT 0,"
		"updated_at INTEGER NOT NULL)",
		"CREATE INDEX IF NOT EXISTS idx_conversations_updated "
		"ON conversations(updated_at DESC,thread_id DESC)",
		"CREATE TABLE IF NOT EXISTS friend_requests("
		"friend_request_id INTEGER PRIMARY KEY,"
		"requester_user_id INTEGER NOT NULL,"
		"target_user_id INTEGER NOT NULL,"
		"request_message TEXT NOT NULL DEFAULT '',"
		"status INTEGER NOT NULL CHECK(status IN(0,1,2)),"
		"thread_id INTEGER NULL,"
		"peer_username TEXT NOT NULL DEFAULT '',"
		"peer_nickname TEXT NOT NULL DEFAULT '',"
		"peer_avatar_key TEXT NOT NULL DEFAULT '',"
		"peer_gender INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE IF NOT EXISTS contacts("
		"user_id INTEGER PRIMARY KEY,"
		"thread_id INTEGER NOT NULL,"
		"username TEXT NOT NULL DEFAULT '',"
		"nickname TEXT NOT NULL DEFAULT '',"
		"avatar_key TEXT NOT NULL DEFAULT '',"
		"gender INTEGER NOT NULL DEFAULT 0)",
		"CREATE TABLE IF NOT EXISTS outbox("
		"operation_id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"client_message_id TEXT NOT NULL UNIQUE,"
		"operation_type TEXT NOT NULL CHECK(operation_type IN('SEND_TEXT','SEND_RESOURCE')),"
		"stage TEXT NOT NULL DEFAULT '',"
		"payload_json TEXT NOT NULL,"
		"FOREIGN KEY(client_message_id) REFERENCES messages(client_message_id) ON DELETE CASCADE)",
		"CREATE TABLE IF NOT EXISTS sync_state("
		"singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),"
		"last_event_seq INTEGER NOT NULL DEFAULT 0 CHECK(last_event_seq>=0),"
		"bootstrap_complete INTEGER NOT NULL DEFAULT 0)",
		"INSERT OR IGNORE INTO sync_state(singleton_id,last_event_seq,bootstrap_complete) "
		"VALUES(1,0,0)"
	};
	for (const char* statement : statements) {
		if (!query.exec(QString::fromLatin1(statement))) {
			qWarning() << "[LocalChatDb] schema failed:" << query.lastError().text()
				<< statement;
			return false;
		}
	}
	return true;
}

LocalMessageDTO LocalChatDb::readMessageRow(QSqlQuery& query) const {
	LocalMessageDTO message;
	message.local_message_id = query.value(0).toLongLong();
	message.message_id = query.value(1).isNull() ? 0 : query.value(1).toLongLong();
	message.client_message_id = query.value(2).toString();
	message.thread_id = query.value(3).toLongLong();
	message.sender_user_id = query.value(4).toLongLong();
	message.message_type = query.value(5).toInt();
	message.text_content = query.value(6).toString();
	message.send_status = query.value(7).toInt();
	message.created_at = query.value(8).toLongLong();
	return message;
}

LocalMessageResourceDTO LocalChatDb::readResourceRow(QSqlQuery& query,
	int firstColumn) const {
	LocalMessageResourceDTO resource;
	if (query.value(firstColumn).isNull()) return resource;
	resource.local_message_id = query.value(firstColumn).toLongLong();
	resource.original_file_name = query.value(firstColumn + 1).toString();
	resource.local_file_path = query.value(firstColumn + 2).toString();
	resource.file_size_bytes = query.value(firstColumn + 3).toLongLong();
	resource.sha256 = query.value(firstColumn + 4).toString();
	resource.mime_type = query.value(firstColumn + 5).toString();
	return resource;
}

bool LocalChatDb::fillMessageByClientId(const QString& clientMessageId,
	LocalMessageDTO* message, LocalMessageResourceDTO* resource) {
	if (!message && !resource) return true;
	QSqlQuery query(_db);
	query.prepare(QStringLiteral("SELECT %1,%2 FROM messages m LEFT JOIN message_resources r "
		"ON r.local_message_id=m.local_message_id WHERE m.client_message_id=?")
		.arg(QString::fromLatin1(kMessageColumns), QString::fromLatin1(kResourceColumns)));
	query.addBindValue(clientMessageId);
	if (!query.exec() || !query.next()) return false;
	if (message) *message = readMessageRow(query);
	if (resource) *resource = readResourceRow(query, 9);
	return true;
}

bool LocalChatDb::enqueueSend(LocalMessageDTO& message,
	LocalMessageResourceDTO& resource, OutboxEntryDTO& outbox) {
	const bool isResource = message.message_type == static_cast<int>(ChatMsgType::PIC)
		|| message.message_type == static_cast<int>(ChatMsgType::FILE);
	if (!isOpen() || message.client_message_id.isEmpty() || message.thread_id <= 0
		|| message.sender_user_id <= 0 || (message.message_type != 0 && !isResource)
		|| outbox.client_message_id != message.client_message_id
		|| outbox.payload_json.isEmpty()
		|| (isResource && (resource.original_file_name.isEmpty()
			|| resource.file_size_bytes <= 0 || resource.sha256.isEmpty()
			|| resource.mime_type.isEmpty()))) {
		return false;
	}
	if (message.created_at <= 0) message.created_at = QDateTime::currentMSecsSinceEpoch();
	message.send_status = LOCAL_SEND_SENDING;
	outbox.operation_type = isResource ? OUTBOX_OP_SEND_RESOURCE : OUTBOX_OP_SEND_TEXT;
	outbox.stage = isResource ? RESOURCE_STAGE_METADATA : QStringLiteral("");

	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	query.prepare(QStringLiteral(
		"INSERT INTO messages(message_id,client_message_id,thread_id,sender_user_id,"
		"message_type,text_content,send_status,created_at) VALUES(NULL,?,?,?,?,?,?,?)"));
	query.addBindValue(message.client_message_id);
	query.addBindValue(message.thread_id);
	query.addBindValue(message.sender_user_id);
	query.addBindValue(message.message_type);
	query.addBindValue(message.message_type == 0
		? NonNullText(message.text_content) : QVariant(QVariant::String));
	query.addBindValue(message.send_status);
	query.addBindValue(message.created_at);
	if (!query.exec()) {
		qWarning() << "[LocalChatDb] enqueue message failed:" << query.lastError().text();
		_db.rollback();
		return false;
	}
	message.local_message_id = query.lastInsertId().toLongLong();

	if (isResource) {
		resource.local_message_id = message.local_message_id;
		query.prepare(QStringLiteral(
			"INSERT INTO message_resources(local_message_id,original_file_name,"
			"local_file_path,file_size_bytes,sha256,mime_type) VALUES(?,?,?,?,?,?)"));
		query.addBindValue(resource.local_message_id);
		query.addBindValue(resource.original_file_name);
		query.addBindValue(NullableText(resource.local_file_path));
		query.addBindValue(resource.file_size_bytes);
		query.addBindValue(resource.sha256);
		query.addBindValue(resource.mime_type);
		if (!query.exec()) {
			_db.rollback();
			return false;
		}
	}

	query.prepare(QStringLiteral(
		"INSERT INTO outbox(client_message_id,operation_type,stage,payload_json) "
		"VALUES(?,?,?,?)"));
	query.addBindValue(outbox.client_message_id);
	query.addBindValue(outbox.operation_type);
	query.addBindValue(NonNullText(outbox.stage));
	query.addBindValue(outbox.payload_json);
	if (!query.exec()) {
		qWarning() << "[LocalChatDb] enqueue outbox failed:" << query.lastError().text();
		_db.rollback();
		return false;
	}
	outbox.operation_id = query.lastInsertId().toLongLong();
	return _db.commit();
}

bool LocalChatDb::confirmTextSent(const QString& clientMessageId, qint64 messageId,
	qint64 createdAt, LocalMessageDTO* out) {
	if (messageId <= 0) return false;
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	query.prepare(QStringLiteral(
		"UPDATE messages SET message_id=?,send_status=?,created_at=? "
		"WHERE client_message_id=? AND message_type=0"));
	query.addBindValue(messageId);
	query.addBindValue(LOCAL_SEND_SENT);
	query.addBindValue(createdAt > 0 ? createdAt : QDateTime::currentMSecsSinceEpoch());
	query.addBindValue(clientMessageId);
	if (!query.exec() || query.numRowsAffected() != 1) {
		_db.rollback();
		return false;
	}
	query.prepare(QStringLiteral("DELETE FROM outbox WHERE client_message_id=?"));
	query.addBindValue(clientMessageId);
	if (!query.exec() || !fillMessageByClientId(clientMessageId, out)) {
		_db.rollback();
		return false;
	}
	return _db.commit();
}

bool LocalChatDb::updateResourceStage(const QString& clientMessageId, qint64 messageId,
	const QString& stage, LocalMessageDTO* out) {
	if (messageId <= 0 || stage != RESOURCE_STAGE_UPLOADING) return false;
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	query.prepare(QStringLiteral(
		"UPDATE messages SET message_id=? WHERE client_message_id=? AND message_type IN(1,3)"));
	query.addBindValue(messageId);
	query.addBindValue(clientMessageId);
	if (!query.exec() || query.numRowsAffected() != 1) {
		_db.rollback();
		return false;
	}
	query.prepare(QStringLiteral("UPDATE outbox SET stage=? WHERE client_message_id=?"));
	query.addBindValue(stage);
	query.addBindValue(clientMessageId);
	if (!query.exec() || query.numRowsAffected() != 1
		|| !fillMessageByClientId(clientMessageId, out)) {
		_db.rollback();
		return false;
	}
	return _db.commit();
}

bool LocalChatDb::confirmResourceSent(const QString& clientMessageId,
	const QString& localFilePath, LocalMessageDTO* out) {
	if (localFilePath.isEmpty()) return false;
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	query.prepare(QStringLiteral(
		"UPDATE message_resources SET local_file_path=? WHERE local_message_id=("
		"SELECT local_message_id FROM messages WHERE client_message_id=?)"));
	query.addBindValue(localFilePath);
	query.addBindValue(clientMessageId);
	if (!query.exec() || query.numRowsAffected() != 1) {
		_db.rollback();
		return false;
	}
	query.prepare(QStringLiteral(
		"UPDATE messages SET send_status=? WHERE client_message_id=? "
		"AND message_type IN(1,3) AND message_id IS NOT NULL"));
	query.addBindValue(LOCAL_SEND_SENT);
	query.addBindValue(clientMessageId);
	if (!query.exec() || query.numRowsAffected() != 1) {
		_db.rollback();
		return false;
	}
	query.prepare(QStringLiteral("DELETE FROM outbox WHERE client_message_id=?"));
	query.addBindValue(clientMessageId);
	if (!query.exec() || !fillMessageByClientId(clientMessageId, out)) {
		_db.rollback();
		return false;
	}
	return _db.commit();
}

bool LocalChatDb::updateResourceLocalPath(qint64 messageId,
	const QString& localFilePath) {
	if (!isOpen() || messageId <= 0 || localFilePath.isEmpty()) return false;
	QSqlQuery query(_db);
	query.prepare(QStringLiteral(
		"UPDATE message_resources SET local_file_path=? WHERE local_message_id=("
		"SELECT local_message_id FROM messages WHERE message_id=?)"));
	query.addBindValue(localFilePath);
	query.addBindValue(messageId);
	return query.exec() && query.numRowsAffected() == 1;
}

bool LocalChatDb::markSendFailed(const QString& clientMessageId,
	LocalMessageDTO* out) {
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	query.prepare(QStringLiteral("UPDATE messages SET send_status=? WHERE client_message_id=?"));
	query.addBindValue(LOCAL_SEND_FAILED);
	query.addBindValue(clientMessageId);
	if (!query.exec() || query.numRowsAffected() != 1) {
		_db.rollback();
		return false;
	}
	query.prepare(QStringLiteral("DELETE FROM outbox WHERE client_message_id=?"));
	query.addBindValue(clientMessageId);
	if (!query.exec() || !fillMessageByClientId(clientMessageId, out)) {
		_db.rollback();
		return false;
	}
	return _db.commit();
}

bool LocalChatDb::getMessageByClientId(const QString& clientMessageId,
	LocalMessageDTO* message, LocalMessageResourceDTO* resource) {
	return isOpen() && fillMessageByClientId(clientMessageId, message, resource);
}

bool LocalChatDb::insertMessageIgnore(const LocalMessageDTO& message,
	const LocalMessageResourceDTO& resource, bool* inserted, qint64* localMessageId) {
	if (message.message_id <= 0 || message.thread_id <= 0 || message.sender_user_id <= 0
		|| (message.message_type != 0 && message.message_type != 1 && message.message_type != 3)) {
		return false;
	}
	QSqlQuery query(_db);
	query.prepare(QStringLiteral(
		"INSERT OR IGNORE INTO messages(message_id,client_message_id,thread_id,sender_user_id,"
		"message_type,text_content,send_status,created_at) VALUES(?,?,?,?,?,?,?,?)"));
	query.addBindValue(message.message_id);
	query.addBindValue(NullableText(message.client_message_id));
	query.addBindValue(message.thread_id);
	query.addBindValue(message.sender_user_id);
	query.addBindValue(message.message_type);
	query.addBindValue(message.message_type == 0
		? NonNullText(message.text_content) : QVariant(QVariant::String));
	query.addBindValue(LOCAL_SEND_SENT);
	query.addBindValue(message.created_at);
	if (!query.exec()) {
		qWarning() << "[LocalChatDb] insert incoming failed:" << query.lastError().text();
		return false;
	}
	const bool wasInserted = query.numRowsAffected() > 0;
	qint64 localId = wasInserted ? query.lastInsertId().toLongLong() : 0;
	if (!wasInserted) {
		query.prepare(QStringLiteral("SELECT local_message_id FROM messages WHERE message_id=?"));
		query.addBindValue(message.message_id);
		if (!query.exec() || !query.next()) return false;
		localId = query.value(0).toLongLong();
	}
	if (inserted) *inserted = wasInserted;
	if (localMessageId) *localMessageId = localId;

	if (message.message_type == 0) return true;
	if (resource.original_file_name.isEmpty() || resource.file_size_bytes <= 0
		|| resource.sha256.isEmpty() || resource.mime_type.isEmpty()) return false;
	query.prepare(QStringLiteral(
		"INSERT INTO message_resources(local_message_id,original_file_name,local_file_path,"
		"file_size_bytes,sha256,mime_type) VALUES(?,?,?,?,?,?) "
		"ON CONFLICT(local_message_id) DO UPDATE SET "
		"original_file_name=excluded.original_file_name,"
		"local_file_path=COALESCE(excluded.local_file_path,message_resources.local_file_path),"
		"file_size_bytes=excluded.file_size_bytes,sha256=excluded.sha256,mime_type=excluded.mime_type"));
	query.addBindValue(localId);
	query.addBindValue(resource.original_file_name);
	query.addBindValue(NullableText(resource.local_file_path));
	query.addBindValue(resource.file_size_bytes);
	query.addBindValue(resource.sha256);
	query.addBindValue(resource.mime_type);
	return query.exec();
}

bool LocalChatDb::upsertConversationOnMessage(const LocalMessageDTO& message) {
	qint64 peerUserId = 0;
	if (message.sender_user_id != _self_user_id) {
		peerUserId = message.sender_user_id;
	} else {
		QSqlQuery existing(_db);
		existing.prepare(QStringLiteral("SELECT peer_user_id FROM conversations WHERE thread_id=?"));
		existing.addBindValue(message.thread_id);
		if (!existing.exec() || !existing.next()) return false;
		peerUserId = existing.value(0).toLongLong();
	}
	if (peerUserId <= 0) return false;
	QSqlQuery query(_db);
	query.prepare(QStringLiteral(
		"INSERT INTO conversations(thread_id,peer_user_id,last_message_id,updated_at) "
		"VALUES(?,?,?,?) "
		"ON CONFLICT(thread_id) DO UPDATE SET peer_user_id=excluded.peer_user_id,"
		"last_message_id=CASE WHEN conversations.last_message_id IS NULL OR "
		"excluded.last_message_id>=conversations.last_message_id THEN excluded.last_message_id "
		"ELSE conversations.last_message_id END,"
		"updated_at=MAX(conversations.updated_at,excluded.updated_at)"));
	query.addBindValue(message.thread_id);
	query.addBindValue(peerUserId);
	query.addBindValue(NullableId(message.message_id));
	query.addBindValue(message.created_at);
	return query.exec();
}

bool LocalChatDb::insertIncoming(const QList<LocalMessageDTO>& messages,
	const QList<LocalMessageResourceDTO>& resources, QList<qint64>* insertedMessageIds) {
	if (messages.size() != resources.size()) return false;
	if (insertedMessageIds) insertedMessageIds->clear();
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	for (int i = 0; i < messages.size(); ++i) {
		bool inserted = false;
		qint64 localId = 0;
		if (!insertMessageIgnore(messages[i], resources[i], &inserted, &localId)
			|| (inserted && !upsertConversationOnMessage(messages[i]))) {
			_db.rollback();
			return false;
		}
		if (inserted && insertedMessageIds) insertedMessageIds->append(messages[i].message_id);
	}
	return _db.commit();
}

bool LocalChatDb::upsertFriendRequest(const LocalFriendRequestDTO& request) {
	if (request.friend_request_id <= 0 || request.requester_user_id <= 0
		|| request.target_user_id <= 0 || request.status < 0 || request.status > 2) return false;
	QSqlQuery query(_db);
	query.prepare(QStringLiteral(
		"INSERT INTO friend_requests(friend_request_id,requester_user_id,target_user_id,"
		"request_message,status,thread_id,peer_username,peer_nickname,peer_avatar_key,peer_gender) "
		"VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(friend_request_id) DO UPDATE SET "
		"requester_user_id=excluded.requester_user_id,target_user_id=excluded.target_user_id,"
		"request_message=excluded.request_message,status=excluded.status,thread_id=excluded.thread_id,"
		"peer_username=excluded.peer_username,peer_nickname=excluded.peer_nickname,"
		"peer_avatar_key=excluded.peer_avatar_key,peer_gender=excluded.peer_gender"));
	query.addBindValue(request.friend_request_id);
	query.addBindValue(request.requester_user_id);
	query.addBindValue(request.target_user_id);
	query.addBindValue(NonNullText(request.request_message));
	query.addBindValue(request.status);
	query.addBindValue(NullableId(request.thread_id));
	query.addBindValue(NonNullText(request.peer_username));
	query.addBindValue(NonNullText(request.peer_nickname));
	query.addBindValue(NonNullText(request.peer_avatar_key));
	query.addBindValue(request.peer_gender);
	return query.exec();
}

bool LocalChatDb::upsertContact(const LocalContactDTO& contact) {
	if (contact.user_id <= 0 || contact.thread_id <= 0) return false;
	QSqlQuery query(_db);
	query.prepare(QStringLiteral(
		"INSERT INTO contacts(user_id,thread_id,username,nickname,avatar_key,gender) "
		"VALUES(?,?,?,?,?,?) ON CONFLICT(user_id) DO UPDATE SET thread_id=excluded.thread_id,"
		"username=excluded.username,nickname=excluded.nickname,avatar_key=excluded.avatar_key,"
		"gender=excluded.gender"));
	query.addBindValue(contact.user_id);
	query.addBindValue(contact.thread_id);
	query.addBindValue(NonNullText(contact.username));
	query.addBindValue(NonNullText(contact.nickname));
	query.addBindValue(NonNullText(contact.avatar_key));
	query.addBindValue(contact.gender);
	return query.exec();
}

bool LocalChatDb::applyEvent(const UserEventDTO& event,
	const QList<LocalMessageDTO>& messages,
	const QList<LocalMessageResourceDTO>& resources,
	const QList<LocalFriendRequestDTO>& friendRequests,
	const QList<LocalContactDTO>& contacts, QList<qint64>* insertedMessageIds) {
	if (event.event_type == 0 || event.event_type == 1 || event.event_type == 3) {
		for (int i = 0; i < messages.size(); ++i) {
			if (messages[i].message_id != event.message_id) continue;
			bool inserted = false;
			qint64 localId = 0;
			if (!insertMessageIgnore(messages[i], resources[i], &inserted, &localId)
				|| (inserted && !upsertConversationOnMessage(messages[i]))) return false;
			if (inserted && insertedMessageIds) insertedMessageIds->append(event.message_id);
			return true;
		}
		return false;
	}

	const LocalFriendRequestDTO* request = nullptr;
	for (const LocalFriendRequestDTO& candidate : friendRequests) {
		if (candidate.friend_request_id == event.friend_request_id) {
			request = &candidate;
			break;
		}
	}
	if (!request || !upsertFriendRequest(*request)) return false;
	if (event.event_type != 11) return event.event_type == 10 || event.event_type == 12;

	const qint64 peerUserId = request->requester_user_id == _self_user_id
		? request->target_user_id : request->requester_user_id;
	QSqlQuery update(_db);
	update.prepare(QStringLiteral(
		"UPDATE friend_requests SET status=1,thread_id=? WHERE status=0 AND "
		"((requester_user_id=? AND target_user_id=?) OR "
		"(requester_user_id=? AND target_user_id=?))"));
	update.addBindValue(request->thread_id);
	update.addBindValue(_self_user_id);
	update.addBindValue(peerUserId);
	update.addBindValue(peerUserId);
	update.addBindValue(_self_user_id);
	if (!update.exec()) return false;

	for (const LocalContactDTO& contact : contacts) {
		if (contact.user_id != peerUserId) continue;
		if (!upsertContact(contact)) return false;
		LocalConversationDTO conversation;
		conversation.thread_id = contact.thread_id;
		conversation.peer_user_id = contact.user_id;
		conversation.updated_at = QDateTime::currentMSecsSinceEpoch();
		return upsertConversations(QList<LocalConversationDTO>() << conversation);
	}
	return false;
}

bool LocalChatDb::applySyncPage(const QList<UserEventDTO>& events,
	const QList<LocalMessageDTO>& messages,
	const QList<LocalMessageResourceDTO>& resources,
	const QList<LocalFriendRequestDTO>& friendRequests,
	const QList<LocalContactDTO>& contacts, qint64 newEventSeq,
	QList<qint64>* insertedMessageIds) {
	if (messages.size() != resources.size()) return false;
	qint64 currentSeq = 0;
	bool bootstrapComplete = false;
	if (!getSyncState(&currentSeq, &bootstrapComplete)) return false;
	Q_UNUSED(bootstrapComplete);
	qint64 expected = currentSeq + 1;
	for (const UserEventDTO& event : events) {
		if (event.event_seq != expected++) return false;
	}
	const qint64 expectedNew = events.isEmpty() ? currentSeq : events.last().event_seq;
	if (newEventSeq != expectedNew) return false;
	if (insertedMessageIds) insertedMessageIds->clear();

	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	for (const UserEventDTO& event : events) {
		if (!applyEvent(event, messages, resources, friendRequests, contacts,
			insertedMessageIds)) {
			_db.rollback();
			return false;
		}
	}
	query.prepare(QStringLiteral("UPDATE sync_state SET last_event_seq=? WHERE singleton_id=1"));
	query.addBindValue(newEventSeq);
	if (!query.exec() || query.numRowsAffected() != 1) {
		_db.rollback();
		return false;
	}
	return _db.commit();
}

bool LocalChatDb::insertHistoryPage(qint64 threadId,
	const QList<LocalMessageDTO>& messages,
	const QList<LocalMessageResourceDTO>& resources, bool historyComplete) {
	if (messages.size() != resources.size()) return false;
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	qint64 oldest = 0;
	for (int i = 0; i < messages.size(); ++i) {
		if (messages[i].thread_id != threadId) {
			_db.rollback();
			return false;
		}
		bool inserted = false;
		qint64 localId = 0;
		if (!insertMessageIgnore(messages[i], resources[i], &inserted, &localId)) {
			_db.rollback();
			return false;
		}
		if (oldest == 0 || messages[i].message_id < oldest) oldest = messages[i].message_id;
	}
	query.prepare(QStringLiteral(
		"UPDATE conversations SET oldest_loaded_message_id=CASE "
		"WHEN oldest_loaded_message_id IS NULL OR oldest_loaded_message_id>? THEN ? "
		"ELSE oldest_loaded_message_id END,history_complete=? WHERE thread_id=?"));
	query.addBindValue(oldest);
	query.addBindValue(oldest);
	query.addBindValue(historyComplete ? 1 : 0);
	query.addBindValue(threadId);
	if (!query.exec() || query.numRowsAffected() != 1) {
		_db.rollback();
		return false;
	}
	return _db.commit();
}

bool LocalChatDb::upsertConversations(
	const QList<LocalConversationDTO>& conversations) {
	QSqlQuery query(_db);
	for (const LocalConversationDTO& conversation : conversations) {
		if (conversation.thread_id <= 0 || conversation.peer_user_id <= 0) return false;
		query.prepare(QStringLiteral(
			"INSERT INTO conversations(thread_id,peer_user_id,last_message_id,"
			"oldest_loaded_message_id,history_complete,updated_at) "
			"VALUES(?,?,?,?,?,?) ON CONFLICT(thread_id) DO UPDATE SET "
			"peer_user_id=excluded.peer_user_id,"
			"last_message_id=COALESCE(excluded.last_message_id,conversations.last_message_id),"
			"oldest_loaded_message_id=COALESCE(excluded.oldest_loaded_message_id,"
			"conversations.oldest_loaded_message_id),"
			"history_complete=MAX(conversations.history_complete,excluded.history_complete),"
			"updated_at=MAX(conversations.updated_at,excluded.updated_at)"));
		query.addBindValue(conversation.thread_id);
		query.addBindValue(conversation.peer_user_id);
		query.addBindValue(NullableId(conversation.last_message_id));
		query.addBindValue(NullableId(conversation.oldest_loaded_message_id));
		query.addBindValue(conversation.history_complete ? 1 : 0);
		query.addBindValue(conversation.updated_at > 0
			? conversation.updated_at : QDateTime::currentMSecsSinceEpoch());
		if (!query.exec()) {
			qWarning() << "[LocalChatDb] upsert conversation failed:"
				<< query.lastError().text();
			return false;
		}
	}
	return true;
}

bool LocalChatDb::applySnapshot(
	const QList<LocalFriendRequestDTO>& friendRequests,
	const QList<LocalContactDTO>& contacts, bool replaceCurrent) {
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
	if (replaceCurrent
		&& (!query.exec(QStringLiteral("DELETE FROM friend_requests"))
			|| !query.exec(QStringLiteral("DELETE FROM contacts")))) {
		_db.rollback();
		return false;
	}
	for (const LocalFriendRequestDTO& request : friendRequests) {
		if (!upsertFriendRequest(request)) {
			_db.rollback();
			return false;
		}
	}
	for (const LocalContactDTO& contact : contacts) {
		if (!upsertContact(contact)) {
			_db.rollback();
			return false;
		}
		LocalConversationDTO conversation;
		conversation.thread_id = contact.thread_id;
		conversation.peer_user_id = contact.user_id;
		conversation.updated_at = QDateTime::currentMSecsSinceEpoch();
		if (!upsertConversations(QList<LocalConversationDTO>() << conversation)) {
			_db.rollback();
			return false;
		}
	}
	return _db.commit();
}

bool LocalChatDb::getSyncState(qint64* lastEventSeq, bool* bootstrapComplete) {
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral(
		"SELECT last_event_seq,bootstrap_complete FROM sync_state WHERE singleton_id=1"))
		|| !query.next()) return false;
	if (lastEventSeq) *lastEventSeq = query.value(0).toLongLong();
	if (bootstrapComplete) *bootstrapComplete = query.value(1).toInt() != 0;
	return true;
}

bool LocalChatDb::markBootstrapComplete(qint64 checkpoint) {
	QSqlQuery query(_db);
	query.prepare(QStringLiteral(
		"UPDATE sync_state SET last_event_seq=?,bootstrap_complete=1 WHERE singleton_id=1"));
	query.addBindValue(checkpoint);
	return query.exec() && query.numRowsAffected() == 1;
}

bool LocalChatDb::loadOutbox(QList<OutboxEntryDTO>* entries) {
	if (!entries) return false;
	entries->clear();
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral(
		"SELECT operation_id,client_message_id,operation_type,stage,payload_json "
		"FROM outbox ORDER BY operation_id ASC"))) return false;
	while (query.next()) {
		OutboxEntryDTO entry;
		entry.operation_id = query.value(0).toLongLong();
		entry.client_message_id = query.value(1).toString();
		entry.operation_type = query.value(2).toString();
		entry.stage = query.value(3).toString();
		entry.payload_json = query.value(4).toString();
		entries->append(entry);
	}
	return true;
}

bool LocalChatDb::loadConversations(
	QList<LocalConversationDTO>* conversations) {
	if (!conversations) return false;
	conversations->clear();
	QSqlQuery query(_db);
	if (!query.exec(QStringLiteral(
		"SELECT thread_id,peer_user_id,last_message_id,"
		"oldest_loaded_message_id,history_complete,updated_at FROM conversations "
		"ORDER BY updated_at DESC,thread_id DESC"))) return false;
	while (query.next()) {
		LocalConversationDTO conversation;
		conversation.thread_id = query.value(0).toLongLong();
		conversation.peer_user_id = query.value(1).toLongLong();
		conversation.last_message_id = query.value(2).isNull() ? 0 : query.value(2).toLongLong();
		conversation.oldest_loaded_message_id = query.value(3).isNull()
			? 0 : query.value(3).toLongLong();
		conversation.history_complete = query.value(4).toInt() != 0;
		conversation.updated_at = query.value(5).toLongLong();
		conversations->append(conversation);
	}
	return true;
}

bool LocalChatDb::loadMessages(const QString& sql, const QVariantList& binds,
	QList<LocalMessageDTO>* messages, QList<LocalMessageResourceDTO>* resources) {
	if (!messages || !resources) return false;
	messages->clear();
	resources->clear();
	QSqlQuery query(_db);
	query.prepare(sql);
	for (const QVariant& bind : binds) query.addBindValue(bind);
	if (!query.exec()) return false;
	while (query.next()) {
		messages->prepend(readMessageRow(query));
		resources->prepend(readResourceRow(query, 9));
	}
	return true;
}

bool LocalChatDb::loadRecentMessages(qint64 threadId, int limit,
	QList<LocalMessageDTO>* messages, QList<LocalMessageResourceDTO>* resources) {
	const QString sql = QStringLiteral(
		"SELECT %1,%2 FROM messages m LEFT JOIN message_resources r "
		"ON r.local_message_id=m.local_message_id WHERE m.thread_id=? "
		"ORDER BY m.created_at DESC,m.local_message_id DESC LIMIT ?")
		.arg(QString::fromLatin1(kMessageColumns), QString::fromLatin1(kResourceColumns));
	return loadMessages(sql, QVariantList() << threadId << limit, messages, resources);
}

bool LocalChatDb::loadOlderMessages(qint64 threadId, qint64 beforeMessageId,
	int limit, QList<LocalMessageDTO>* messages,
	QList<LocalMessageResourceDTO>* resources) {
	const QString sql = QStringLiteral(
		"SELECT %1,%2 FROM messages m LEFT JOIN message_resources r "
		"ON r.local_message_id=m.local_message_id WHERE m.thread_id=? "
		"AND m.message_id IS NOT NULL AND m.message_id<? "
		"ORDER BY m.message_id DESC LIMIT ?")
		.arg(QString::fromLatin1(kMessageColumns), QString::fromLatin1(kResourceColumns));
	return loadMessages(sql, QVariantList() << threadId << beforeMessageId << limit,
		messages, resources);
}
