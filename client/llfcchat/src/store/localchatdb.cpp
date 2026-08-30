#include "localchatdb.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QMap>
#include <QPair>
#include <QSet>
#include <QUuid>
#include "global.h"

//messages 表列清单（readMessageRow 与各 SELECT 共用）
static const char* MSG_COLUMNS =
	"local_id, server_message_id, recv_seq, client_message_id, thread_id, sender_id, receiver_id,"
    " message_type, content, local_path, content_size, resource_status, content_hash,"
    " mime_type, send_state, created_at";

LocalChatDb::LocalChatDb(const QString& conn_name)
    : _conn_name(conn_name.isEmpty() ? QUuid::createUuid().toString() : conn_name)
{
}

LocalChatDb::~LocalChatDb()
{
    close();
    //释放副本后再移除命名连接，避免 Qt 警告
    _db = QSqlDatabase();
    QSqlDatabase::removeDatabase(_conn_name);
}

bool LocalChatDb::open(const QString& dbPath, qint64 selfUid)
{
    _self_uid = selfUid;
    _db = QSqlDatabase::addDatabase("QSQLITE", _conn_name);
    _db.setDatabaseName(dbPath);
    if (!_db.open()) {
        qWarning() << "[LocalChatDb] open failed:" << _db.lastError().text();
        return false;
    }

    QSqlQuery pragma(_db);
    //WAL + FULL 同步 + 外键 + 5s 忙等（计划 §4.2）
    if (!pragma.exec("PRAGMA journal_mode=WAL;")
        || !pragma.exec("PRAGMA synchronous=FULL;")
        || !pragma.exec("PRAGMA foreign_keys=ON;")
        || !pragma.exec("PRAGMA busy_timeout=5000;")) {
        qWarning() << "[LocalChatDb] pragma failed:" << pragma.lastError().text();
        return false;
    }

	if (!pragma.exec("PRAGMA user_version") || !pragma.next()) {
		return false;
	}
	const int schema_version = pragma.value(0).toInt();
	if (schema_version != 3) {
		if (!_db.transaction()) return false;
		for (const char* table : { "messages", "conversations", "outbox",
			"sync_state", "friend_requests", "contacts" }) {
			if (!pragma.exec(QString("DROP TABLE IF EXISTS %1").arg(table))) {
				_db.rollback();
				return false;
			}
		}
		if (!_db.commit()) return false;
	}
	if (!initSchema()) return false;
	return pragma.exec("PRAGMA user_version=3");
}

void LocalChatDb::close()
{
    if (_db.isValid() && _db.isOpen()) {
        _db.close();
    }
}

bool LocalChatDb::isOpen() const
{
    return _db.isValid() && _db.isOpen();
}

bool LocalChatDb::initSchema()
{
    QSqlQuery query(_db);
    //server_message_id 可 NULL，SQLite 唯一索引容许多个 NULL（0=未确认不落盘）
    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS messages ("
        " local_id INTEGER PRIMARY KEY AUTOINCREMENT,"
		" server_message_id INTEGER UNIQUE,"
		" recv_seq INTEGER UNIQUE,"
        " client_message_id TEXT NOT NULL UNIQUE,"
		" thread_id INTEGER,"
        " sender_id INTEGER NOT NULL,"
        " receiver_id INTEGER NOT NULL,"
        " message_type INTEGER NOT NULL,"
        " content TEXT,"
        " local_path TEXT,"
        " content_size TEXT,"
        " resource_status INTEGER NOT NULL DEFAULT 0,"
        " content_hash TEXT,"
        " mime_type TEXT,"
        " send_state TEXT NOT NULL DEFAULT 'sending',"
        " created_at TEXT)")) {
        qWarning() << "[LocalChatDb] create messages failed:" << query.lastError().text();
        return false;
    }
    if (!query.exec(
        "CREATE INDEX IF NOT EXISTS idx_messages_thread"
        " ON messages(thread_id, server_message_id)")) {
        qWarning() << "[LocalChatDb] create index failed:" << query.lastError().text();
        return false;
    }
    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS conversations ("
        " thread_id INTEGER PRIMARY KEY,"
        " peer_uid INTEGER NOT NULL,"
        " last_server_message_id INTEGER NOT NULL DEFAULT 0,"
        " last_message_preview TEXT,"
        " unread_count INTEGER NOT NULL DEFAULT 0,"
        " oldest_loaded_message_id INTEGER NOT NULL DEFAULT 0,"
        " history_complete INTEGER NOT NULL DEFAULT 0,"
        " updated_at TEXT)")) {
        qWarning() << "[LocalChatDb] create conversations failed:" << query.lastError().text();
        return false;
    }
    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS outbox ("
        " operation_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " operation_type TEXT NOT NULL,"
        " dedup_key TEXT NOT NULL UNIQUE,"
        " request_id TEXT,"
        " payload TEXT,"
        " stage TEXT)")) {
        qWarning() << "[LocalChatDb] create outbox failed:" << query.lastError().text();
        return false;
    }
	if (!query.exec(
		"CREATE TABLE IF NOT EXISTS friend_requests ("
		" message_id INTEGER PRIMARY KEY, from_uid INTEGER NOT NULL, to_uid INTEGER NOT NULL,"
		" direction INTEGER NOT NULL, business_status INTEGER NOT NULL, description TEXT,"
		" requester_remark TEXT, related_thread_id INTEGER, handled_at TEXT,"
		" name TEXT, nick TEXT, icon TEXT, sex INTEGER, profile_desc TEXT, created_at TEXT)")) {
		return false;
	}
	if (!query.exec(
		"CREATE TABLE IF NOT EXISTS contacts ("
		" uid INTEGER PRIMARY KEY, thread_id INTEGER, name TEXT, nick TEXT, icon TEXT,"
		" sex INTEGER, profile_desc TEXT, remark TEXT, updated_at TEXT)")) {
		return false;
	}
    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS sync_state ("
        " id INTEGER PRIMARY KEY CHECK (id = 1),"
		" last_recv_seq INTEGER NOT NULL DEFAULT 0,"
        " bootstrap_complete INTEGER NOT NULL DEFAULT 0)")) {
        qWarning() << "[LocalChatDb] create sync_state failed:" << query.lastError().text();
        return false;
    }
    //sync_state 单行占位（id=1）
	if (!query.exec("INSERT OR IGNORE INTO sync_state(id, last_recv_seq, bootstrap_complete)"
        " VALUES(1, 0, 0)")) {
        qWarning() << "[LocalChatDb] init sync_state failed:" << query.lastError().text();
        return false;
    }
    return true;
}

LocalMessageDTO LocalChatDb::readMessageRow(QSqlQuery& query)
{
    LocalMessageDTO dto;
    dto.local_id = query.value(0).toLongLong();
    //server_message_id 可能为 NULL（未确认），NULL 语义回读为 0
    dto.server_message_id = query.value(1).isNull() ? 0 : query.value(1).toLongLong();
	dto.recv_seq = query.value(2).isNull() ? 0 : query.value(2).toLongLong();
	dto.client_message_id = query.value(3).toString();
	dto.thread_id = query.value(4).toLongLong();
	dto.sender_id = query.value(5).toLongLong();
	dto.receiver_id = query.value(6).toLongLong();
	dto.message_type = query.value(7).toInt();
	dto.content = query.value(8).toString();
	dto.local_path = query.value(9).toString();
	dto.content_size = query.value(10).toString();
	dto.resource_status = query.value(11).toInt();
	dto.content_hash = query.value(12).toString();
	dto.mime_type = query.value(13).toString();
	dto.send_state = query.value(14).toString();
	dto.created_at = query.value(15).toString();
    return dto;
}

bool LocalChatDb::fillMessageByClientId(const QString& clientMessageId, LocalMessageDTO* out)
{
    if (!out) {
        return true;
    }
    QSqlQuery query(_db);
    query.prepare(QString("SELECT %1 FROM messages WHERE client_message_id = :cid")
        .arg(MSG_COLUMNS));
    query.bindValue(":cid", clientMessageId);
    if (!query.exec() || !query.next()) {
        return false;
    }
    *out = readMessageRow(query);
    return true;
}

bool LocalChatDb::insertMessageIgnore(const LocalMessageDTO& dto, bool* inserted)
{
    QSqlQuery query(_db);
    query.prepare(
        "INSERT OR IGNORE INTO messages"
		" (server_message_id, recv_seq, client_message_id, thread_id, sender_id, receiver_id,"
        "  message_type, content, local_path, content_size, resource_status, content_hash,"
        "  mime_type, send_state, created_at)"
		" VALUES(:sid, :rseq, :cid, :tid, :sender, :recv, :mtype, :content, :lpath, :csize,"
        "  :rstatus, :chash, :mime, :state, :ctime)");
    //0=未确认语义落盘为 NULL，避免唯一索引把多条 0 判重
    if (dto.server_message_id > 0) {
        query.bindValue(":sid", QVariant::fromValue<qint64>(dto.server_message_id));
    } else {
        query.bindValue(":sid", QVariant(QVariant::LongLong));
    }
	if (dto.recv_seq > 0) query.bindValue(":rseq", dto.recv_seq);
	else query.bindValue(":rseq", QVariant(QVariant::LongLong));
    query.bindValue(":cid", dto.client_message_id);
    query.bindValue(":tid", QVariant::fromValue<qint64>(dto.thread_id));
    query.bindValue(":sender", QVariant::fromValue<qint64>(dto.sender_id));
    query.bindValue(":recv", QVariant::fromValue<qint64>(dto.receiver_id));
    query.bindValue(":mtype", dto.message_type);
    query.bindValue(":content", dto.content);
    query.bindValue(":lpath", dto.local_path);
    query.bindValue(":csize", dto.content_size);
    query.bindValue(":rstatus", dto.resource_status);
    query.bindValue(":chash", dto.content_hash);
    query.bindValue(":mime", dto.mime_type);
    query.bindValue(":state", dto.send_state.isEmpty() ? SEND_STATE_SENT : dto.send_state);
    query.bindValue(":ctime", dto.created_at);
    if (!query.exec()) {
        qWarning() << "[LocalChatDb] insert message failed:" << query.lastError().text();
        return false;
    }
    if (inserted) {
        *inserted = query.numRowsAffected() > 0;
    }
    return true;
}

bool LocalChatDb::upsertConversationOnMessage(const LocalMessageDTO& dto, bool incoming)
{
    qint64 peer_uid = (dto.sender_id == _self_uid) ? dto.receiver_id : dto.sender_id;
    QString preview;
    if (dto.message_type == static_cast<int>(ChatMsgType::PIC)) {
        preview = QStringLiteral("[图片]");
    } else if (dto.message_type == static_cast<int>(ChatMsgType::FILE)) {
        preview = QStringLiteral("[文件]");
    } else {
        preview = dto.content;
    }
    int unread_inc = (incoming && dto.sender_id != _self_uid) ? 1 : 0;
    QString ts = QDateTime::currentDateTime().toString(Qt::ISODate);

    QSqlQuery query(_db);
    query.prepare(
        "INSERT INTO conversations"
        " (thread_id, peer_uid, last_server_message_id, last_message_preview, unread_count, updated_at)"
        " VALUES(:tid, :peer, :last, :preview, :unread, :ts)"
        " ON CONFLICT(thread_id) DO UPDATE SET"
        " peer_uid = excluded.peer_uid,"
        " last_server_message_id = MAX(last_server_message_id, excluded.last_server_message_id),"
        " last_message_preview = excluded.last_message_preview,"
        " unread_count = unread_count + excluded.unread_count,"
        " updated_at = excluded.updated_at");
    query.bindValue(":tid", QVariant::fromValue<qint64>(dto.thread_id));
    query.bindValue(":peer", QVariant::fromValue<qint64>(peer_uid));
    query.bindValue(":last", QVariant::fromValue<qint64>(dto.server_message_id));
    query.bindValue(":preview", preview);
    query.bindValue(":unread", unread_inc);
    query.bindValue(":ts", ts);
    if (!query.exec()) {
        qWarning() << "[LocalChatDb] upsert conversation failed:" << query.lastError().text();
        return false;
    }
    return true;
}

bool LocalChatDb::enqueueSend(LocalMessageDTO& dto, OutboxEntryDTO* outEntry)
{
    if (!isOpen()) {
        return false;
    }
    if (dto.created_at.isEmpty()) {
        dto.created_at = QDateTime::currentDateTime().toString(Qt::ISODate);
    }
    dto.send_state = SEND_STATE_SENDING;

    //组装重发所需原始请求 payload（与 ChatPage 既有 1301/1503 格式一致）
    //资源消息（图片/文件）要求 content_hash/mime_type 已由后台哈希填好（磁盘可能变化，落库时定格）
    const bool is_resource = dto.message_type == static_cast<int>(ChatMsgType::PIC)
        || dto.message_type == static_cast<int>(ChatMsgType::FILE);
    QJsonObject payload;
    payload["fromuid"] = dto.sender_id;
    payload["touid"] = dto.receiver_id;
    //thread_id/content_size 按字符串化协议输出十进制字符串（dispatcher 续传也按字符串回读）
    payload["thread_id"] = QString::number(dto.thread_id);
    payload["unique_id"] = dto.client_message_id;
    if (is_resource) {
        payload["msg_type"] = dto.message_type;
        payload["file_name"] = dto.content;
        payload["content_hash"] = dto.content_hash;
        payload["mime_type"] = dto.mime_type;
        payload["text_or_url"] = dto.local_path;
        payload["content_size"] = dto.content_size;
    } else {
        payload["content"] = dto.content;
    }
    QString payload_str = QString::fromUtf8(
        QJsonDocument(payload).toJson(QJsonDocument::Compact));

    if (!_db.transaction()) {
        return false;
    }

    QSqlQuery query(_db);
    query.prepare(
        "INSERT INTO messages"
        " (server_message_id, client_message_id, thread_id, sender_id, receiver_id,"
        "  message_type, content, local_path, content_size, resource_status, content_hash,"
        "  mime_type, send_state, created_at)"
        " VALUES(NULL, :cid, :tid, :sender, :recv, :mtype, :content, :lpath, :csize,"
        "  :rstatus, :chash, :mime, :state, :ctime)");
    query.bindValue(":cid", dto.client_message_id);
    query.bindValue(":tid", QVariant::fromValue<qint64>(dto.thread_id));
    query.bindValue(":sender", QVariant::fromValue<qint64>(dto.sender_id));
    query.bindValue(":recv", QVariant::fromValue<qint64>(dto.receiver_id));
    query.bindValue(":mtype", dto.message_type);
    query.bindValue(":content", dto.content);
    query.bindValue(":lpath", dto.local_path);
    query.bindValue(":csize", dto.content_size);
    query.bindValue(":rstatus", dto.resource_status);
    query.bindValue(":chash", dto.content_hash);
    query.bindValue(":mime", dto.mime_type);
    query.bindValue(":state", dto.send_state);
    query.bindValue(":ctime", dto.created_at);
    if (!query.exec()) {
        qWarning() << "[LocalChatDb] enqueue insert failed:" << query.lastError().text();
        _db.rollback();
        return false;
    }
    dto.local_id = query.lastInsertId().toLongLong();

    QSqlQuery ob(_db);
    ob.prepare(
        "INSERT INTO outbox (operation_type, dedup_key, request_id, payload, stage)"
        " VALUES(:op, :dedup, :req, :payload, :stage)");
    ob.bindValue(":op", is_resource ? OUTBOX_OP_SEND_RESOURCE : OUTBOX_OP_SEND_TEXT);
    ob.bindValue(":dedup", dto.client_message_id);
    ob.bindValue(":req", dto.client_message_id);
    ob.bindValue(":payload", payload_str);
    //资源初始阶段 metadata（等待 1504），文本不需要阶段
    const QString outbox_stage = is_resource ? RESOURCE_STAGE_METADATA : QString();
    ob.bindValue(":stage", outbox_stage);
    if (!ob.exec()) {
        qWarning() << "[LocalChatDb] enqueue outbox failed:" << ob.lastError().text();
        _db.rollback();
        return false;
    }
    const qint64 outbox_operation_id = ob.lastInsertId().toLongLong();

    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    //commit 成功后才产出"已提交事实"：磁盘保存的内容与网络发送的内容同源
    if (outEntry) {
        outEntry->operation_id = outbox_operation_id;
        outEntry->operation_type = is_resource ? OUTBOX_OP_SEND_RESOURCE : OUTBOX_OP_SEND_TEXT;
        outEntry->dedup_key = dto.client_message_id;
        outEntry->request_id = dto.client_message_id;
        outEntry->payload = payload_str;
        outEntry->stage = outbox_stage;
    }
    return true;
}

bool LocalChatDb::confirmTextSent(const QString& clientMessageId, qint64 serverMessageId,
    const QString& chatTime, LocalMessageDTO* out)
{
    if (!isOpen()) {
        return false;
    }
    if (!_db.transaction()) {
        return false;
    }
    QSqlQuery query(_db);
    query.prepare(
        "UPDATE messages SET server_message_id = :sid, send_state = :state, created_at = :ctime"
        " WHERE client_message_id = :cid");
    query.bindValue(":sid", QVariant::fromValue<qint64>(serverMessageId));
    query.bindValue(":state", SEND_STATE_SENT);
    query.bindValue(":ctime", chatTime);
    query.bindValue(":cid", clientMessageId);
    if (!query.exec()) {
        _db.rollback();
        return false;
    }
    QSqlQuery ob(_db);
    ob.prepare("DELETE FROM outbox WHERE dedup_key = :dedup");
    ob.bindValue(":dedup", clientMessageId);
    if (!ob.exec()) {
        _db.rollback();
        return false;
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return fillMessageByClientId(clientMessageId, out);
}

bool LocalChatDb::updateResourceStage(const QString& clientMessageId, qint64 serverMessageId,
    const QString& stage, LocalMessageDTO* out)
{
    if (!isOpen()) {
        return false;
    }
    if (!_db.transaction()) {
        return false;
    }
    QSqlQuery query(_db);
    query.prepare("UPDATE messages SET server_message_id = :sid WHERE client_message_id = :cid");
    query.bindValue(":sid", QVariant::fromValue<qint64>(serverMessageId));
    query.bindValue(":cid", clientMessageId);
    if (!query.exec()) {
        _db.rollback();
        return false;
    }
    //只推进 outbox 阶段，不删 outbox（1506 完成才删）
    QSqlQuery ob(_db);
    ob.prepare("UPDATE outbox SET stage = :stage WHERE dedup_key = :dedup");
    ob.bindValue(":stage", stage);
    ob.bindValue(":dedup", clientMessageId);
    if (!ob.exec()) {
        _db.rollback();
        return false;
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return fillMessageByClientId(clientMessageId, out);
}

bool LocalChatDb::confirmResourceSent(const QString& clientMessageId, LocalMessageDTO* out)
{
    if (!isOpen()) {
        return false;
    }
    if (!_db.transaction()) {
        return false;
    }
    QSqlQuery query(_db);
    query.prepare("UPDATE messages SET send_state = :state WHERE client_message_id = :cid");
    query.bindValue(":state", SEND_STATE_SENT);
    query.bindValue(":cid", clientMessageId);
    if (!query.exec()) {
        _db.rollback();
        return false;
    }
    QSqlQuery ob(_db);
    ob.prepare("DELETE FROM outbox WHERE dedup_key = :dedup");
    ob.bindValue(":dedup", clientMessageId);
    if (!ob.exec()) {
        _db.rollback();
        return false;
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return fillMessageByClientId(clientMessageId, out);
}

bool LocalChatDb::markSendFailed(const QString& clientMessageId, LocalMessageDTO* out)
{
    if (!isOpen()) {
        return false;
    }
    if (!_db.transaction()) {
        return false;
    }
    QSqlQuery query(_db);
    query.prepare("UPDATE messages SET send_state = :state WHERE client_message_id = :cid");
    query.bindValue(":state", SEND_STATE_FAILED);
    query.bindValue(":cid", clientMessageId);
    if (!query.exec()) {
        _db.rollback();
        return false;
    }
    QSqlQuery ob(_db);
    ob.prepare("DELETE FROM outbox WHERE dedup_key = :dedup");
    ob.bindValue(":dedup", clientMessageId);
    if (!ob.exec()) {
        _db.rollback();
        return false;
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return fillMessageByClientId(clientMessageId, out);
}

bool LocalChatDb::getMessageByClientId(const QString& clientMessageId, LocalMessageDTO* out)
{
    if (!isOpen() || !out) {
        return false;
    }
    return fillMessageByClientId(clientMessageId, out);
}

bool LocalChatDb::applyUnifiedMessage(const LocalMessageDTO& dto, bool* inserted)
{
	if (inserted) *inserted = false;
	const int apply_type = static_cast<int>(ChatMsgType::FRIEND_APPLY);
	const int accept_type = static_cast<int>(ChatMsgType::FRIEND_ACCEPT);
	const int reject_type = static_cast<int>(ChatMsgType::FRIEND_REJECT);
	if (dto.message_type == apply_type) {
		QSqlQuery query(_db);
		query.prepare(
			"INSERT OR IGNORE INTO friend_requests"
			" (message_id, from_uid, to_uid, direction, business_status, description,"
			" requester_remark, name, nick, icon, sex, profile_desc, created_at)"
			" VALUES(:mid,:from,:to,:direction,:status,:description,:remark,"
			" :name,:nick,:icon,:sex,:profile_desc,:created)");
		query.bindValue(":mid", dto.server_message_id);
		query.bindValue(":from", dto.sender_id);
		query.bindValue(":to", dto.receiver_id);
		query.bindValue(":direction", dto.sender_id == _self_uid ? 1 : 0);
		query.bindValue(":status", dto.business_status);
		query.bindValue(":description", dto.content);
		query.bindValue(":remark", dto.requester_remark);
		query.bindValue(":name", dto.sender_name);
		query.bindValue(":nick", dto.sender_nick);
		query.bindValue(":icon", dto.sender_icon);
		query.bindValue(":sex", dto.sender_sex);
		query.bindValue(":profile_desc", dto.sender_desc);
		query.bindValue(":created", dto.created_at);
		if (!query.exec()) return false;
		if (inserted) *inserted = query.numRowsAffected() > 0;
		return true;
	}

	if (dto.message_type == accept_type || dto.message_type == reject_type) {
		QSqlQuery update(_db);
		update.prepare(
			"UPDATE friend_requests SET business_status=:status, handled_at=:handled,"
			" related_thread_id=:thread WHERE message_id=:related OR "
			"(:accepted=1 AND business_status=1 AND "
			"((from_uid=:from AND to_uid=:to) OR (from_uid=:to AND to_uid=:from)))");
		update.bindValue(":status", dto.business_status);
		update.bindValue(":handled", dto.handled_at);
		update.bindValue(":thread", dto.thread_id > 0
			? QVariant::fromValue<qint64>(dto.thread_id) : QVariant(QVariant::LongLong));
		update.bindValue(":related", dto.related_message_id);
		update.bindValue(":accepted", dto.message_type == accept_type ? 1 : 0);
		update.bindValue(":from", QVariant::fromValue<qint64>(dto.sender_id));
		update.bindValue(":to", QVariant::fromValue<qint64>(dto.receiver_id));
		if (!update.exec()) return false;
		if (dto.message_type == reject_type) {
			if (inserted) *inserted = true;
			return true;
		}

		const qint64 peer_uid = dto.sender_id == _self_uid ? dto.receiver_id : dto.sender_id;
		QSqlQuery contact(_db);
		contact.prepare(
			"INSERT INTO contacts(uid,thread_id,name,nick,icon,sex,profile_desc,remark,updated_at)"
			" VALUES(:uid,:thread,:name,:nick,:icon,:sex,:profile_desc,:remark,:updated)"
			" ON CONFLICT(uid) DO UPDATE SET thread_id=excluded.thread_id,name=excluded.name,"
			" nick=excluded.nick,icon=excluded.icon,sex=excluded.sex,"
			" profile_desc=excluded.profile_desc,remark=excluded.remark,updated_at=excluded.updated_at");
		contact.bindValue(":uid", peer_uid);
		contact.bindValue(":thread", dto.thread_id);
		contact.bindValue(":name", dto.sender_name);
		contact.bindValue(":nick", dto.sender_nick);
		contact.bindValue(":icon", dto.sender_icon);
		contact.bindValue(":sex", dto.sender_sex);
		contact.bindValue(":profile_desc", dto.sender_desc);
		contact.bindValue(":remark", dto.requester_remark);
		contact.bindValue(":updated", dto.created_at);
		if (!contact.exec()) return false;
	}

	bool message_inserted = false;
	if (!insertMessageIgnore(dto, &message_inserted)) return false;
	if (message_inserted && dto.thread_id > 0 && !upsertConversationOnMessage(dto, true)) {
		return false;
	}
	if (inserted) *inserted = message_inserted;
	return true;
}

bool LocalChatDb::insertIncoming(const QList<LocalMessageDTO>& msgs, QList<qint64>* insertedIds)
{
    if (!isOpen()) {
        return false;
    }
    if (insertedIds) {
        insertedIds->clear();
    }
    if (!_db.transaction()) {
        return false;
    }
    for (const LocalMessageDTO& dto : msgs) {
        bool inserted = false;
		if (!applyUnifiedMessage(dto, &inserted)) {
            _db.rollback();
            return false;
        }
        if (!inserted) {
            //重复消息：UI 去重与会话未读都已在首次插入时处理
            continue;
        }
		if (insertedIds && dto.server_message_id > 0 &&
			(dto.message_type <= static_cast<int>(ChatMsgType::FILE) ||
			 dto.message_type == static_cast<int>(ChatMsgType::FRIEND_APPLY) ||
			 dto.message_type == static_cast<int>(ChatMsgType::FRIEND_ACCEPT))) {
            insertedIds->append(dto.server_message_id);
        }
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return true;
}

bool LocalChatDb::applySyncPage(const QList<LocalMessageDTO>& msgs, qint64 newSyncSeq,
    QList<qint64>* insertedIds)
{
    if (!isOpen()) {
        return false;
    }
    if (insertedIds) {
        insertedIds->clear();
    }
    if (!_db.transaction()) {
        return false;
    }
    for (const LocalMessageDTO& dto : msgs) {
        bool inserted = false;
		if (!applyUnifiedMessage(dto, &inserted)) {
            _db.rollback();
            return false;
        }
        if (!inserted) {
            continue;
        }
		if (insertedIds && dto.server_message_id > 0 &&
			(dto.message_type <= static_cast<int>(ChatMsgType::FILE) ||
			 dto.message_type == static_cast<int>(ChatMsgType::FRIEND_APPLY) ||
			 dto.message_type == static_cast<int>(ChatMsgType::FRIEND_ACCEPT))) {
            insertedIds->append(dto.server_message_id);
        }
    }
    //整页写完后推进游标（同事务，失败整体回滚游标不动）
    QSqlQuery sync(_db);
	sync.prepare("UPDATE sync_state SET last_recv_seq = :seq WHERE id = 1");
    sync.bindValue(":seq", QVariant::fromValue<qint64>(newSyncSeq));
    if (!sync.exec()) {
        _db.rollback();
        return false;
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return true;
}

bool LocalChatDb::insertHistoryPage(qint64 threadId, const QList<LocalMessageDTO>& msgs,
    bool historyComplete)
{
    if (!isOpen()) {
        return false;
    }
    if (!_db.transaction()) {
        return false;
    }
    qint64 oldest = 0;
    qint64 peer_uid = 0;
    for (const LocalMessageDTO& dto : msgs) {
        bool inserted = false;
        if (!insertMessageIgnore(dto, &inserted)) {
            _db.rollback();
            return false;
        }
        if (dto.server_message_id > 0 && (oldest == 0 || dto.server_message_id < oldest)) {
            oldest = dto.server_message_id;
        }
        if (peer_uid == 0) {
            peer_uid = (dto.sender_id == _self_uid) ? dto.receiver_id : dto.sender_id;
        }
    }
    //会话行可能尚不存在，先补占位再更新游标
    QSqlQuery ins(_db);
    ins.prepare("INSERT OR IGNORE INTO conversations(thread_id, peer_uid) VALUES(:tid, :peer)");
    ins.bindValue(":tid", QVariant::fromValue<qint64>(threadId));
    ins.bindValue(":peer", QVariant::fromValue<qint64>(peer_uid));
    if (!ins.exec()) {
        _db.rollback();
        return false;
    }
    QSqlQuery upd(_db);
    upd.prepare(
        "UPDATE conversations SET"
        " oldest_loaded_message_id = CASE"
        "  WHEN :oldest > 0 AND (oldest_loaded_message_id = 0 OR :oldest < oldest_loaded_message_id)"
        "  THEN :oldest ELSE oldest_loaded_message_id END,"
        " history_complete = MAX(history_complete, :complete)"
        " WHERE thread_id = :tid");
    upd.bindValue(":oldest", QVariant::fromValue<qint64>(oldest));
    upd.bindValue(":complete", historyComplete ? 1 : 0);
    upd.bindValue(":tid", QVariant::fromValue<qint64>(threadId));
    if (!upd.exec()) {
        _db.rollback();
        return false;
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return true;
}

bool LocalChatDb::upsertConversations(const QList<LocalConversationDTO>& convs)
{
    if (!isOpen()) {
        return false;
    }
    if (!_db.transaction()) {
        return false;
    }
    for (const LocalConversationDTO& conv : convs) {
        QSqlQuery query(_db);
        query.prepare(
            "INSERT INTO conversations (thread_id, peer_uid, last_server_message_id, updated_at)"
            " VALUES(:tid, :peer, :last, :ts)"
            " ON CONFLICT(thread_id) DO UPDATE SET"
            " peer_uid = excluded.peer_uid,"
            " last_server_message_id = MAX(last_server_message_id, excluded.last_server_message_id),"
            " updated_at = excluded.updated_at");
        query.bindValue(":tid", QVariant::fromValue<qint64>(conv.thread_id));
        query.bindValue(":peer", QVariant::fromValue<qint64>(conv.peer_uid));
        query.bindValue(":last", QVariant::fromValue<qint64>(conv.last_server_message_id));
        query.bindValue(":ts", conv.updated_at.isEmpty()
            ? QDateTime::currentDateTime().toString(Qt::ISODate) : conv.updated_at);
        if (!query.exec()) {
            _db.rollback();
            return false;
        }
    }
    if (!_db.commit()) {
        _db.rollback();
        return false;
    }
    return true;
}

bool LocalChatDb::applySnapshot(const QJsonArray& friendRequests,
	const QJsonArray& contacts, bool replaceCurrent)
{
	if (!isOpen() || !_db.transaction()) return false;
	QSqlQuery query(_db);
	QSet<qint64> current_pending_ids;
	QMap<qint64, qint64> current_contact_threads;
	if (replaceCurrent &&
		(!query.exec("DELETE FROM friend_requests") || !query.exec("DELETE FROM contacts"))) {
		_db.rollback();
		return false;
	}
	for (const QJsonValue& value : friendRequests) {
		const QJsonObject obj = value.toObject();
		current_pending_ids.insert(obj["message_id"].toString().toLongLong());
		query.prepare(
			"INSERT INTO friend_requests(message_id,from_uid,to_uid,direction,business_status,"
			" description,requester_remark,name,nick,icon,sex,profile_desc,created_at)"
			" VALUES(:mid,:from,:to,:direction,:status,:description,:remark,:name,:nick,"
			" :icon,:sex,:profile_desc,:created) ON CONFLICT(message_id) DO UPDATE SET"
			" from_uid=excluded.from_uid,to_uid=excluded.to_uid,direction=excluded.direction,"
			" business_status=excluded.business_status,description=excluded.description,"
			" requester_remark=excluded.requester_remark,name=excluded.name,nick=excluded.nick,"
			" icon=excluded.icon,sex=excluded.sex,profile_desc=excluded.profile_desc,"
			" created_at=excluded.created_at");
		query.bindValue(":mid", obj["message_id"].toString().toLongLong());
		query.bindValue(":from", obj["from_uid"].toInt());
		query.bindValue(":to", obj["to_uid"].toInt());
		query.bindValue(":direction", obj["from_uid"].toInt() == _self_uid ? 1 : 0);
		query.bindValue(":status", obj["status"].toInt());
		query.bindValue(":description", obj["desc"].toString());
		query.bindValue(":remark", obj["requester_remark"].toString());
		query.bindValue(":name", obj["name"].toString());
		query.bindValue(":nick", obj["nick"].toString());
		query.bindValue(":icon", obj["icon"].toString());
		query.bindValue(":sex", obj["sex"].toInt());
		query.bindValue(":profile_desc", obj["profile_desc"].toString());
		query.bindValue(":created", obj["created_at"].toString());
		if (!query.exec()) { _db.rollback(); return false; }
	}
	for (const QJsonValue& value : contacts) {
		const QJsonObject obj = value.toObject();
		current_contact_threads.insert(
			obj["uid"].toInt(), obj["thread_id"].toString().toLongLong());
		query.prepare(
			"INSERT INTO contacts(uid,thread_id,name,nick,icon,sex,profile_desc,remark,updated_at)"
			" VALUES(:uid,:thread,:name,:nick,:icon,:sex,:profile_desc,:remark,:updated)"
			" ON CONFLICT(uid) DO UPDATE SET thread_id=excluded.thread_id,name=excluded.name,"
			" nick=excluded.nick,icon=excluded.icon,sex=excluded.sex,"
			" profile_desc=excluded.profile_desc,remark=excluded.remark,updated_at=excluded.updated_at");
		query.bindValue(":uid", obj["uid"].toInt());
		query.bindValue(":thread", obj["thread_id"].toString().toLongLong());
		query.bindValue(":name", obj["name"].toString());
		query.bindValue(":nick", obj["nick"].toString());
		query.bindValue(":icon", obj["icon"].toString());
		query.bindValue(":sex", obj["sex"].toInt());
		query.bindValue(":profile_desc", obj["desc"].toString());
		query.bindValue(":remark", obj["back"].toString());
		query.bindValue(":updated", QDateTime::currentDateTime().toString(Qt::ISODate));
		if (!query.exec()) { _db.rollback(); return false; }
	}
	if (!replaceCurrent) {
		QSqlQuery pending(_db);
		if (!pending.exec(
			"SELECT message_id,from_uid,to_uid FROM friend_requests WHERE business_status=1")) {
			_db.rollback();
			return false;
		}
		QList<QPair<qint64, qint64>> stale_pending;
		while (pending.next()) {
			const qint64 message_id = pending.value(0).toLongLong();
			if (current_pending_ids.contains(message_id)) continue;
			const qint64 from_uid = pending.value(1).toLongLong();
			const qint64 to_uid = pending.value(2).toLongLong();
			const qint64 peer_uid = from_uid == _self_uid ? to_uid : from_uid;
			stale_pending.append(qMakePair(message_id, peer_uid));
		}
		pending.finish();
		for (const auto& stale : stale_pending) {
			const qint64 message_id = stale.first;
			const qint64 peer_uid = stale.second;
			const bool accepted = current_contact_threads.contains(peer_uid);
			QSqlQuery reconcile(_db);
			reconcile.prepare(
				"UPDATE friend_requests SET business_status=:status,related_thread_id=:thread "
				"WHERE message_id=:mid AND business_status=1");
			reconcile.bindValue(":status", accepted ? 2 : 3);
			if (accepted) {
				reconcile.bindValue(":thread", QVariant::fromValue<qint64>(
					current_contact_threads.value(peer_uid)));
			} else {
				reconcile.bindValue(":thread", QVariant(QVariant::LongLong));
			}
			reconcile.bindValue(":mid", QVariant::fromValue<qint64>(message_id));
			if (!reconcile.exec()) { _db.rollback(); return false; }
		}
	}
	if (!_db.commit()) { _db.rollback(); return false; }
	return true;
}

bool LocalChatDb::getSyncState(qint64* lastRecvSeq, bool* bootstrapComplete)
{
    if (!isOpen()) {
        return false;
    }
    QSqlQuery query(_db);
	if (!query.exec("SELECT last_recv_seq, bootstrap_complete FROM sync_state WHERE id = 1")
        || !query.next()) {
        return false;
    }
	if (lastRecvSeq) {
		*lastRecvSeq = query.value(0).toLongLong();
    }
    if (bootstrapComplete) {
        *bootstrapComplete = query.value(1).toInt() != 0;
    }
    return true;
}

bool LocalChatDb::markBootstrapComplete(qint64 checkpoint)
{
    if (!isOpen()) {
        return false;
    }
    //单行 id=1 UPSERT：置 bootstrap 完成并记录 checkpoint 游标
    QSqlQuery query(_db);
    query.prepare(
		"INSERT INTO sync_state(id, last_recv_seq, bootstrap_complete) VALUES(1, :seq, 1)"
        " ON CONFLICT(id) DO UPDATE SET"
		" last_recv_seq = excluded.last_recv_seq, bootstrap_complete = 1");
    query.bindValue(":seq", QVariant::fromValue<qint64>(checkpoint));
    if (!query.exec()) {
        return false;
    }
    return true;
}

bool LocalChatDb::loadOutbox(QList<OutboxEntryDTO>* entries)
{
    if (!isOpen() || !entries) {
        return false;
    }
    entries->clear();
    QSqlQuery query(_db);
    if (!query.exec(
        "SELECT operation_id, operation_type, dedup_key, request_id, payload, stage"
        " FROM outbox ORDER BY operation_id ASC")) {
        return false;
    }
    while (query.next()) {
        OutboxEntryDTO entry;
        entry.operation_id = query.value(0).toLongLong();
        entry.operation_type = query.value(1).toString();
        entry.dedup_key = query.value(2).toString();
        entry.request_id = query.value(3).toString();
        entry.payload = query.value(4).toString();
        entry.stage = query.value(5).toString();
        entries->append(entry);
    }
    return true;
}

bool LocalChatDb::loadConversations(QList<LocalConversationDTO>* convs)
{
    if (!isOpen() || !convs) {
        return false;
    }
    convs->clear();
    QSqlQuery query(_db);
    if (!query.exec(
        "SELECT thread_id, peer_uid, last_server_message_id, last_message_preview,"
        " unread_count, oldest_loaded_message_id, history_complete, updated_at"
        " FROM conversations ORDER BY updated_at DESC")) {
        return false;
    }
    while (query.next()) {
        LocalConversationDTO conv;
        conv.thread_id = query.value(0).toLongLong();
        conv.peer_uid = query.value(1).toLongLong();
        conv.last_server_message_id = query.value(2).toLongLong();
        conv.last_message_preview = query.value(3).toString();
        conv.unread_count = query.value(4).toInt();
        conv.oldest_loaded_message_id = query.value(5).toLongLong();
        conv.history_complete = query.value(6).toInt() != 0;
        conv.updated_at = query.value(7).toString();
        convs->append(conv);
    }
    return true;
}

bool LocalChatDb::loadRecentMessages(qint64 threadId, int limit, QList<LocalMessageDTO>* msgs)
{
    if (!isOpen() || !msgs) {
        return false;
    }
    msgs->clear();
    QSqlQuery query(_db);
    query.prepare(QString("SELECT %1 FROM messages WHERE thread_id = :tid"
        " ORDER BY local_id DESC LIMIT :limit").arg(MSG_COLUMNS));
    query.bindValue(":tid", QVariant::fromValue<qint64>(threadId));
    query.bindValue(":limit", limit);
    if (!query.exec()) {
        return false;
    }
    while (query.next()) {
        msgs->prepend(readMessageRow(query));
    }
    return true;
}

bool LocalChatDb::loadOlderMessages(qint64 threadId, qint64 beforeMessageId, int limit,
    QList<LocalMessageDTO>* msgs)
{
    if (!isOpen() || !msgs) {
        return false;
    }
    msgs->clear();
    QSqlQuery query(_db);
    query.prepare(QString("SELECT %1 FROM messages WHERE thread_id = :tid"
        " AND server_message_id IS NOT NULL AND server_message_id < :before"
        " ORDER BY server_message_id DESC LIMIT :limit").arg(MSG_COLUMNS));
    query.bindValue(":tid", QVariant::fromValue<qint64>(threadId));
    query.bindValue(":before", QVariant::fromValue<qint64>(beforeMessageId));
    query.bindValue(":limit", limit);
    if (!query.exec()) {
        return false;
    }
    while (query.next()) {
        msgs->prepend(readMessageRow(query));
    }
    return true;
}
