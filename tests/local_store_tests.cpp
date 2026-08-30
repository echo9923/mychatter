// LocalChatDb tests for the destructive seven-table client schema.

#include "localchatdb.h"

#include <QCoreApplication>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>

#include <cstdio>

namespace {

const qint64 kSelf = 1001;
const qint64 kPeer = 2002;
const qint64 kThread = 35;

int gFailures = 0;
int gConnectionSequence = 0;

void Check(bool condition, const char* name) {
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++gFailures;
    }
}

QVariant SqlValue(const QString& dbPath, const QString& sql) {
    const QString connection = QStringLiteral("local_store_check_%1")
        .arg(++gConnectionSequence);
    QVariant value;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(sql) && query.next()) value = query.value(0);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return value;
}

QSet<QString> TableColumns(const QString& dbPath, const QString& table) {
    const QString connection = QStringLiteral("local_store_columns_%1")
        .arg(++gConnectionSequence);
    QSet<QString> columns;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
                while (query.next()) columns.insert(query.value(1).toString());
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return columns;
}

LocalConversationDTO Conversation(qint64 threadId = kThread,
    qint64 peerUserId = kPeer) {
    LocalConversationDTO conversation;
    conversation.thread_id = threadId;
    conversation.peer_user_id = peerUserId;
    conversation.updated_at = 1000;
    return conversation;
}

LocalMessageDTO OutgoingText(const QString& clientMessageId) {
    LocalMessageDTO message;
    message.client_message_id = clientMessageId;
    message.thread_id = kThread;
    message.sender_user_id = kSelf;
    message.message_type = 0;
    message.text_content = QStringLiteral("hello");
    message.created_at = 1000;
    return message;
}

LocalMessageDTO IncomingText(qint64 messageId, qint64 createdAt = 2000) {
    LocalMessageDTO message;
    message.message_id = messageId;
    message.thread_id = kThread;
    message.sender_user_id = kPeer;
    message.message_type = 0;
    message.text_content = QStringLiteral("incoming");
    message.created_at = createdAt;
    return message;
}

LocalMessageDTO IncomingResource(qint64 messageId, int messageType,
    qint64 createdAt = 3000) {
    LocalMessageDTO message;
    message.message_id = messageId;
    message.thread_id = kThread;
    message.sender_user_id = kPeer;
    message.message_type = messageType;
    message.created_at = createdAt;
    return message;
}

LocalMessageResourceDTO Resource(const QString& path = QString()) {
    LocalMessageResourceDTO resource;
    resource.original_file_name = QStringLiteral("photo.png");
    resource.local_file_path = path;
    resource.file_size_bytes = 4096;
    resource.sha256 = QString(64, QLatin1Char('a'));
    resource.mime_type = QStringLiteral("image/png");
    return resource;
}

OutboxEntryDTO Outbox(const QString& clientMessageId) {
    OutboxEntryDTO entry;
    entry.client_message_id = clientMessageId;
    entry.payload_json = QStringLiteral("{\"client_message_id\":\"%1\"}")
        .arg(clientMessageId);
    return entry;
}

void TestSchema(LocalChatDb& db, const QString& dbPath) {
    Check(db.isOpen(), "database opened");
    Check(SqlValue(dbPath, QStringLiteral("PRAGMA user_version")).toInt() == 4,
        "schema version is 4");
    Check(SqlValue(dbPath, QStringLiteral("PRAGMA journal_mode")).toString()
        .compare(QStringLiteral("wal"), Qt::CaseInsensitive) == 0,
        "journal mode is WAL");
    Check(SqlValue(dbPath, QStringLiteral(
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%'")).toInt() == 7,
        "schema has exactly seven business tables");

    const QSet<QString> messageColumns = TableColumns(dbPath, QStringLiteral("messages"));
    const QSet<QString> expectedMessageColumns = QSet<QString>()
        << QStringLiteral("local_message_id") << QStringLiteral("message_id")
        << QStringLiteral("client_message_id") << QStringLiteral("thread_id")
        << QStringLiteral("sender_user_id") << QStringLiteral("message_type")
        << QStringLiteral("text_content") << QStringLiteral("send_status")
        << QStringLiteral("created_at");
    Check(messageColumns == expectedMessageColumns,
        "messages contains only current chat fields");

    const QSet<QString> resourceColumns =
        TableColumns(dbPath, QStringLiteral("message_resources"));
    Check(resourceColumns.contains(QStringLiteral("local_message_id"))
        && resourceColumns.contains(QStringLiteral("local_file_path"))
        && resourceColumns.contains(QStringLiteral("file_size_bytes"))
        && !resourceColumns.contains(QStringLiteral("resource_status")),
        "resource fields live only in message_resources");
    Check(!messageColumns.contains(QStringLiteral("recv_seq")),
        "messages has no event cursor");
    Check(!TableColumns(dbPath, QStringLiteral("friend_requests"))
        .contains(QStringLiteral("remark")),
        "friend requests have no remark field");
}

void TestTextOutbox(LocalChatDb& db, const QString& dbPath) {
    const LocalConversationDTO seedConversation = Conversation();
    Check(db.upsertConversations(QList<LocalConversationDTO>() << seedConversation),
        "seed private conversation");

    LocalMessageDTO message = OutgoingText(QStringLiteral("text-1"));
    LocalMessageResourceDTO resource;
    OutboxEntryDTO outbox = Outbox(message.client_message_id);
    Check(db.enqueueSend(message, resource, outbox), "enqueue text atomically");
    Check(message.local_message_id > 0 && outbox.operation_id > 0,
        "enqueue returns local and operation ids");
    Check(outbox.operation_type == OUTBOX_OP_SEND_TEXT && outbox.stage.isEmpty(),
        "text outbox has only SEND_TEXT state");

    LocalMessageDTO stored;
    LocalMessageResourceDTO storedResource;
    Check(db.getMessageByClientId(message.client_message_id, &stored, &storedResource)
        && stored.message_id == 0 && stored.send_status == LOCAL_SEND_SENDING
        && storedResource.local_message_id == 0,
        "pending text has no resource row or server id");

    QList<OutboxEntryDTO> entries;
    Check(db.loadOutbox(&entries) && entries.size() == 1
        && entries.first().client_message_id == message.client_message_id,
        "outbox persists client_message_id directly");

    const qint64 messageId = 5000000001LL;
    const qint64 createdAt = 1788065723000LL;
    LocalMessageDTO confirmed;
    Check(db.confirmTextSent(message.client_message_id, messageId, createdAt, &confirmed),
        "confirm text commits");
    Check(confirmed.message_id == messageId
        && confirmed.send_status == LOCAL_SEND_SENT
        && confirmed.created_at == createdAt,
        "confirm writes canonical id, status and created_at");
    Check(db.loadOutbox(&entries) && entries.isEmpty(),
        "confirm text removes outbox row");

    LocalMessageDTO failedMessage = OutgoingText(QStringLiteral("text-failed"));
    OutboxEntryDTO failedOutbox = Outbox(failedMessage.client_message_id);
    Check(db.enqueueSend(failedMessage, resource, failedOutbox),
        "enqueue second text");
    Check(db.markSendFailed(failedMessage.client_message_id, &stored)
        && stored.send_status == LOCAL_SEND_FAILED,
        "permanent send failure is persisted");
    Check(SqlValue(dbPath, QStringLiteral(
        "SELECT COUNT(*) FROM outbox WHERE client_message_id='text-failed'")).toInt() == 0,
        "failed send removes outbox row");
}

void TestResourceOutbox(LocalChatDb& db) {
    LocalMessageDTO message;
    message.client_message_id = QStringLiteral("resource-1");
    message.thread_id = kThread;
    message.sender_user_id = kSelf;
    message.message_type = 1;
    message.created_at = 4000;
    LocalMessageResourceDTO resource = Resource(QStringLiteral("C:/source/photo.png"));
    OutboxEntryDTO outbox = Outbox(message.client_message_id);

    Check(db.enqueueSend(message, resource, outbox), "enqueue resource atomically");
    Check(resource.local_message_id == message.local_message_id
        && outbox.operation_type == OUTBOX_OP_SEND_RESOURCE
        && outbox.stage == RESOURCE_STAGE_METADATA,
        "resource row and metadata stage are assigned");

    LocalMessageDTO staged;
    Check(db.updateResourceStage(message.client_message_id, 6000000001LL,
        RESOURCE_STAGE_UPLOADING, &staged)
        && staged.message_id == 6000000001LL,
        "1504 stores message_id and uploading stage");

    QList<OutboxEntryDTO> entries;
    Check(db.loadOutbox(&entries) && entries.size() == 1
        && entries.first().stage == RESOURCE_STAGE_UPLOADING,
        "uploading stage remains durable");

    const QString archivePath = QStringLiteral("C:/cache/6000000001");
    LocalMessageDTO confirmed;
    Check(db.confirmResourceSent(message.client_message_id, archivePath, &confirmed)
        && confirmed.send_status == LOCAL_SEND_SENT,
        "1506 confirms resource and archives local path");

    LocalMessageResourceDTO storedResource;
    Check(db.getMessageByClientId(message.client_message_id, &staged, &storedResource)
        && storedResource.local_file_path == archivePath,
        "sender archive path is persisted in resource extension");
    Check(db.loadOutbox(&entries) && entries.isEmpty(),
        "resource confirmation removes outbox row");
}

void TestIncomingAndHistory(LocalChatDb& db, const QString& dbPath) {
    QList<LocalMessageDTO> messages;
    QList<LocalMessageResourceDTO> resources;
    messages << IncomingText(7000000001LL);
    resources << LocalMessageResourceDTO();
    messages << IncomingResource(7000000002LL, 1);
    resources << Resource();

    QList<qint64> inserted;
    Check(db.insertIncoming(messages, resources, &inserted)
        && inserted.size() == 2,
        "incoming text and resource are inserted");
    Check(db.insertIncoming(messages, resources, &inserted) && inserted.isEmpty(),
        "duplicate message_id is idempotent");

    QList<LocalConversationDTO> conversations;
    Check(db.loadConversations(&conversations) && conversations.size() == 1
        && conversations.first().last_message_id == 7000000002LL
        && conversations.first().last_message_preview == QStringLiteral("[图片]")
        && conversations.first().unread_count == 2,
        "incoming messages update preview and unread count once");

    const QString downloadedPath = QStringLiteral("C:/cache/7000000002/photo.png");
    Check(db.updateResourceLocalPath(7000000002LL, downloadedPath),
        "download completion updates resource local path");

    QList<LocalMessageDTO> recent;
    QList<LocalMessageResourceDTO> recentResources;
    Check(db.loadRecentMessages(kThread, 20, &recent, &recentResources)
        && recent.size() >= 2 && recent.size() == recentResources.size(),
        "recent messages preserve message/resource alignment");
    bool foundResource = false;
    for (int i = 0; i < recent.size(); ++i) {
        if (recent.at(i).message_id != 7000000002LL) continue;
        foundResource = recentResources.at(i).local_file_path == downloadedPath;
    }
    Check(foundResource, "loaded resource carries persisted cache path");

    const int unreadBeforeHistory = SqlValue(dbPath, QStringLiteral(
        "SELECT unread_count FROM conversations WHERE thread_id=35")).toInt();
    QList<LocalMessageDTO> history;
    QList<LocalMessageResourceDTO> historyResources;
    history << IncomingText(6999999999LL, 100);
    historyResources << LocalMessageResourceDTO();
    Check(db.insertHistoryPage(kThread, history, historyResources, true),
        "history page is inserted");
    Check(SqlValue(dbPath, QStringLiteral(
        "SELECT unread_count FROM conversations WHERE thread_id=35")).toInt()
        == unreadBeforeHistory,
        "history does not increase unread count");
    Check(SqlValue(dbPath, QStringLiteral(
        "SELECT history_complete FROM conversations WHERE thread_id=35")).toInt() == 1,
        "history completion is persisted");
}

void TestFriendEvents(LocalChatDb& db, const QString& dbPath) {
    LocalFriendRequestDTO pending;
    pending.friend_request_id = 8000000001LL;
    pending.requester_user_id = kPeer;
    pending.target_user_id = kSelf;
    pending.request_message = QStringLiteral("please add me");
    pending.status = 0;
    pending.peer_username = QStringLiteral("peer");
    pending.peer_nickname = QStringLiteral("Peer");
    pending.peer_avatar_key = QStringLiteral("head_1.jpg");
    pending.peer_gender = 1;

    UserEventDTO applyEvent;
    applyEvent.event_seq = 1;
    applyEvent.event_type = 10;
    applyEvent.friend_request_id = pending.friend_request_id;
    QList<qint64> inserted;
    Check(db.applySyncPage(QList<UserEventDTO>() << applyEvent,
        QList<LocalMessageDTO>(), QList<LocalMessageResourceDTO>(),
        QList<LocalFriendRequestDTO>() << pending, QList<LocalContactDTO>(),
        1, &inserted),
        "friend request event applies");

    const int messageCountBefore = SqlValue(dbPath,
        QStringLiteral("SELECT COUNT(*) FROM messages")).toInt();
    Check(inserted.isEmpty(), "friend request inserts no message id");

    LocalFriendRequestDTO accepted = pending;
    accepted.status = 1;
    accepted.thread_id = 36;
    LocalContactDTO contact;
    contact.user_id = kPeer;
    contact.thread_id = accepted.thread_id;
    contact.username = pending.peer_username;
    contact.nickname = pending.peer_nickname;
    contact.avatar_key = pending.peer_avatar_key;
    contact.gender = pending.peer_gender;

    UserEventDTO acceptedEvent;
    acceptedEvent.event_seq = 2;
    acceptedEvent.event_type = 11;
    acceptedEvent.friend_request_id = accepted.friend_request_id;
    Check(db.applySyncPage(QList<UserEventDTO>() << acceptedEvent,
        QList<LocalMessageDTO>(), QList<LocalMessageResourceDTO>(),
        QList<LocalFriendRequestDTO>() << accepted,
        QList<LocalContactDTO>() << contact, 2, &inserted),
        "friend accepted event applies atomically");
    Check(SqlValue(dbPath, QStringLiteral("SELECT COUNT(*) FROM messages")).toInt()
        == messageCountBefore,
        "friend events never enter messages");
    Check(SqlValue(dbPath, QStringLiteral(
        "SELECT status FROM friend_requests WHERE friend_request_id=8000000001")).toInt() == 1,
        "friend request status becomes accepted");
    Check(SqlValue(dbPath, QStringLiteral(
        "SELECT COUNT(*) FROM contacts WHERE user_id=2002 AND thread_id=36")).toInt() == 1,
        "accepted friend becomes contact");
    Check(SqlValue(dbPath, QStringLiteral(
        "SELECT COUNT(*) FROM conversations WHERE thread_id=36 AND peer_user_id=2002")).toInt() == 1,
        "accepted friend gets private conversation");

    qint64 lastEventSeq = 0;
    bool bootstrapComplete = false;
    Check(db.getSyncState(&lastEventSeq, &bootstrapComplete)
        && lastEventSeq == 2 && !bootstrapComplete,
        "friend business and event cursor commit together");

    UserEventDTO gap = acceptedEvent;
    gap.event_seq = 4;
    Check(!db.applySyncPage(QList<UserEventDTO>() << gap,
        QList<LocalMessageDTO>(), QList<LocalMessageResourceDTO>(),
        QList<LocalFriendRequestDTO>() << accepted,
        QList<LocalContactDTO>() << contact, 4, &inserted),
        "event gap is rejected");
    Check(db.getSyncState(&lastEventSeq, &bootstrapComplete) && lastEventSeq == 2,
        "rejected gap does not advance cursor");

    Check(db.markBootstrapComplete(5000000000LL)
        && db.getSyncState(&lastEventSeq, &bootstrapComplete)
        && lastEventSeq == 5000000000LL && bootstrapComplete,
        "64-bit event checkpoint round-trips");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    Check(temporary.isValid(), "temporary directory created");
    if (!temporary.isValid()) return 1;

    const QString dbPath = temporary.filePath(QStringLiteral("chat.db"));
    LocalChatDb db(QStringLiteral("local_store_test"));
    Check(db.open(dbPath, kSelf), "open LocalChatDb");
    if (!db.isOpen()) return 1;

    TestSchema(db, dbPath);
    TestTextOutbox(db, dbPath);
    TestResourceOutbox(db);
    TestIncomingAndHistory(db, dbPath);
    TestFriendEvents(db, dbPath);

    std::printf("\n%s: %d failure(s)\n",
        gFailures == 0 ? "ALL PASS" : "FAILED", gFailures);
    return gFailures == 0 ? 0 : 1;
}
