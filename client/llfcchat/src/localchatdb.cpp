#include "localchatdb.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDateTime>
#include <QUuid>
#include "global.h"

//messages 表列清单（readMessageRow 与各 SELECT 共用）
static const char* MSG_COLUMNS =
    "local_id, server_message_id, client_message_id, thread_id, sender_id, receiver_id,"
    " message_type, content, local_path, content_size, send_state, created_at";

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

    return initSchema();
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
        " client_message_id TEXT NOT NULL UNIQUE,"
        " thread_id INTEGER NOT NULL,"
        " sender_id INTEGER NOT NULL,"
        " receiver_id INTEGER NOT NULL,"
        " message_type INTEGER NOT NULL,"
        " content TEXT,"
        " local_path TEXT,"
        " content_size TEXT,"
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
        " stage TEXT,"
        " retry_count INTEGER NOT NULL DEFAULT 0,"
        " next_retry_at INTEGER NOT NULL DEFAULT 0)")) {
        qWarning() << "[LocalChatDb] create outbox failed:" << query.lastError().text();
        return false;
    }
    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS sync_state ("
        " id INTEGER PRIMARY KEY CHECK (id = 1),"
        " last_sync_seq INTEGER NOT NULL DEFAULT 0,"
        " bootstrap_complete INTEGER NOT NULL DEFAULT 0)")) {
        qWarning() << "[LocalChatDb] create sync_state failed:" << query.lastError().text();
        return false;
    }
    //sync_state 单行占位（id=1）
    if (!query.exec("INSERT OR IGNORE INTO sync_state(id, last_sync_seq, bootstrap_complete)"
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
    dto.client_message_id = query.value(2).toString();
    dto.thread_id = query.value(3).toLongLong();
    dto.sender_id = query.value(4).toLongLong();
    dto.receiver_id = query.value(5).toLongLong();
    dto.message_type = query.value(6).toInt();
    dto.content = query.value(7).toString();
    dto.local_path = query.value(8).toString();
    dto.content_size = query.value(9).toString();
    dto.send_state = query.value(10).toString();
    dto.created_at = query.value(11).toString();
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
        " (server_message_id, client_message_id, thread_id, sender_id, receiver_id,"
        "  message_type, content, local_path, content_size, send_state, created_at)"
        " VALUES(:sid, :cid, :tid, :sender, :recv, :mtype, :content, :lpath, :csize, :state, :ctime)");
    //0=未确认语义落盘为 NULL，避免唯一索引把多条 0 判重
    if (dto.server_message_id > 0) {
        query.bindValue(":sid", QVariant::fromValue<qint64>(dto.server_message_id));
    } else {
        query.bindValue(":sid", QVariant(QVariant::LongLong));
    }
    query.bindValue(":cid", dto.client_message_id);
    query.bindValue(":tid", QVariant::fromValue<qint64>(dto.thread_id));
    query.bindValue(":sender", QVariant::fromValue<qint64>(dto.sender_id));
    query.bindValue(":recv", QVariant::fromValue<qint64>(dto.receiver_id));
    query.bindValue(":mtype", dto.message_type);
    query.bindValue(":content", dto.content);
    query.bindValue(":lpath", dto.local_path);
    query.bindValue(":csize", dto.content_size);
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
    QString preview = (dto.message_type == 1) ? QStringLiteral("[图片]") : dto.content;
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

bool LocalChatDb::enqueueSend(LocalMessageDTO& dto)
{
    if (!isOpen()) {
        return false;
    }
    if (dto.created_at.isEmpty()) {
        dto.created_at = QDateTime::currentDateTime().toString(Qt::ISODate);
    }
    dto.send_state = SEND_STATE_SENDING;

    //组装重发所需原始请求 payload（与 ChatPage 既有 1017/1035 格式一致）
    bool is_image = dto.message_type == 1;
    QJsonObject payload;
    payload["fromuid"] = dto.sender_id;
    payload["touid"] = dto.receiver_id;
    //thread_id/content_size 按字符串化协议输出十进制字符串（dispatcher 续传也按字符串回读）
    payload["thread_id"] = QString::number(dto.thread_id);
    payload["unique_id"] = dto.client_message_id;
    if (is_image) {
        payload["md5"] = calculateFileHash(dto.local_path);
        payload["name"] = dto.content;
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
        "  message_type, content, local_path, content_size, send_state, created_at)"
        " VALUES(NULL, :cid, :tid, :sender, :recv, :mtype, :content, :lpath, :csize, :state, :ctime)");
    query.bindValue(":cid", dto.client_message_id);
    query.bindValue(":tid", QVariant::fromValue<qint64>(dto.thread_id));
    query.bindValue(":sender", QVariant::fromValue<qint64>(dto.sender_id));
    query.bindValue(":recv", QVariant::fromValue<qint64>(dto.receiver_id));
    query.bindValue(":mtype", dto.message_type);
    query.bindValue(":content", dto.content);
    query.bindValue(":lpath", dto.local_path);
    query.bindValue(":csize", dto.content_size);
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
    ob.bindValue(":op", is_image ? OUTBOX_OP_SEND_IMAGE : OUTBOX_OP_SEND_TEXT);
    ob.bindValue(":dedup", dto.client_message_id);
    ob.bindValue(":req", dto.client_message_id);
    ob.bindValue(":payload", payload_str);
    //图片初始阶段 metadata（等待 1036），文本不需要阶段
    ob.bindValue(":stage", is_image ? IMG_STAGE_METADATA : QString());
    if (!ob.exec()) {
        qWarning() << "[LocalChatDb] enqueue outbox failed:" << ob.lastError().text();
        _db.rollback();
        return false;
    }

    if (!_db.commit()) {
        _db.rollback();
        return false;
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

bool LocalChatDb::updateImageStage(const QString& clientMessageId, qint64 serverMessageId,
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
    //只推进 outbox 阶段，不删 outbox（1038 完成才删）
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

bool LocalChatDb::confirmImageSent(const QString& clientMessageId, LocalMessageDTO* out)
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
        if (!insertMessageIgnore(dto, &inserted)) {
            _db.rollback();
            return false;
        }
        if (!inserted) {
            //重复消息：UI 去重与会话未读都已在首次插入时处理
            continue;
        }
        if (insertedIds && dto.server_message_id > 0) {
            insertedIds->append(dto.server_message_id);
        }
        if (!upsertConversationOnMessage(dto, true)) {
            _db.rollback();
            return false;
        }
        //接收成功生成 DELIVERY_ACK outbox（幂等：重复投递 INSERT OR IGNORE 无操作）
        if (dto.server_message_id > 0 && dto.sender_id != _self_uid) {
            QJsonObject ack_payload;
            ack_payload["message_id"] = QString::number(dto.server_message_id);
            QSqlQuery ob(_db);
            ob.prepare(
                "INSERT OR IGNORE INTO outbox (operation_type, dedup_key, payload)"
                " VALUES(:op, :dedup, :payload)");
            ob.bindValue(":op", OUTBOX_OP_DELIVERY_ACK);
            ob.bindValue(":dedup", "ack_" + QString::number(dto.server_message_id));
            ob.bindValue(":payload", QString::fromUtf8(
                QJsonDocument(ack_payload).toJson(QJsonDocument::Compact)));
            if (!ob.exec()) {
                _db.rollback();
                return false;
            }
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
        if (!insertMessageIgnore(dto, &inserted)) {
            _db.rollback();
            return false;
        }
        if (!inserted) {
            continue;
        }
        if (insertedIds && dto.server_message_id > 0) {
            insertedIds->append(dto.server_message_id);
        }
        if (!upsertConversationOnMessage(dto, true)) {
            _db.rollback();
            return false;
        }
    }
    //整页写完后推进游标（同事务，失败整体回滚游标不动）
    QSqlQuery sync(_db);
    sync.prepare("UPDATE sync_state SET last_sync_seq = :seq WHERE id = 1");
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

bool LocalChatDb::getSyncState(qint64* lastSyncSeq, bool* bootstrapComplete)
{
    if (!isOpen()) {
        return false;
    }
    QSqlQuery query(_db);
    if (!query.exec("SELECT last_sync_seq, bootstrap_complete FROM sync_state WHERE id = 1")
        || !query.next()) {
        return false;
    }
    if (lastSyncSeq) {
        *lastSyncSeq = query.value(0).toLongLong();
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
        "INSERT INTO sync_state(id, last_sync_seq, bootstrap_complete) VALUES(1, :seq, 1)"
        " ON CONFLICT(id) DO UPDATE SET"
        " last_sync_seq = excluded.last_sync_seq, bootstrap_complete = 1");
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
        "SELECT operation_id, operation_type, dedup_key, request_id, payload, stage,"
        " retry_count, next_retry_at FROM outbox ORDER BY operation_id ASC")) {
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
        entry.retry_count = query.value(6).toInt();
        entry.next_retry_at = query.value(7).toLongLong();
        entries->append(entry);
    }
    return true;
}

bool LocalChatDb::deleteOutboxEntry(const QString& dedupKey)
{
    if (!isOpen()) {
        return false;
    }
    QSqlQuery query(_db);
    query.prepare("DELETE FROM outbox WHERE dedup_key = :dedup");
    query.bindValue(":dedup", dedupKey);
    return query.exec();
}

bool LocalChatDb::updateOutboxRetry(const QString& dedupKey, int retryCount, qint64 nextRetryAt)
{
    if (!isOpen()) {
        return false;
    }
    QSqlQuery query(_db);
    query.prepare("UPDATE outbox SET retry_count = :rc, next_retry_at = :next"
        " WHERE dedup_key = :dedup");
    query.bindValue(":rc", retryCount);
    query.bindValue(":next", QVariant::fromValue<qint64>(nextRetryAt));
    query.bindValue(":dedup", dedupKey);
    return query.exec();
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
