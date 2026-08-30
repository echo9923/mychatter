// local_store_tests — LocalChatDb (client SQLite local store) unit tests.
//
// No test framework: plain main() prints [PASS]/[FAIL] and exits non-zero on
// any failure (mirrors worker_pool_tests / password_hash_tests). Compiles the
// production localchatdb.cpp + global.cpp directly (see tests/CMakeLists.txt)
// and links Qt5 Core/Sql only for store logic; Gui/Network/Widgets come along
// because localchatdb.cpp includes the client's global.h.
//
// LocalChatDb is synchronous and not a QObject, so no event loop is needed;
// a QCoreApplication is still created for proper Qt/plugin initialization.
// Every group gets its own QTemporaryDir + chat.db for full isolation.
//
// QSqlDatabase hygiene: LocalChatDb uses a unique named connection per
// instance (auto UUID) and removes it in its destructor, so crash-recovery
// groups use a fresh instance per reopen. Direct SQL verification goes through
// SqlScalar(), which drops its QSqlDatabase copy before removeDatabase() so
// Qt never prints "connection still in use" warnings.

#include "localchatdb.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <QFile>
#include <QList>
#include <QString>
#include <QtGlobal>

#include <cstdio>

namespace {

const qint64 kSelf = 1001;   //本端 uid
const qint64 kPeer = 2002;   //对端 uid
const qint64 kThread = 100;  //默认私聊 thread

int g_failures = 0;
int g_conn_seq = 0;          //校验连接命名序列
qint64 g_client_seq = 0;     //client_message_id 生成序列

void Check(bool cond, const char* name)
{
    if (cond) { std::printf("[PASS] %s\n", name); }
    else { std::printf("[FAIL] %s\n", name); ++g_failures; }
}

void CheckDetail(bool cond, const char* name, const QString& detail)
{
    if (cond) { std::printf("[PASS] %s\n", name); }
    else {
        std::printf("[FAIL] %s\n", name);
        std::printf("      detail: %s\n", qPrintable(detail));
        ++g_failures;
    }
}

//独立只读校验连接：作用域内 close 后再 removeDatabase，避免 Qt 告警
bool SqlScalar(const QString& dbPath, const QString& sql, QString* out)
{
    const QString conn = QString("local_store_check_%1").arg(++g_conn_seq);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(sql) && query.next()) {
                if (out) {
                    *out = query.value(0).toString();
                }
                ok = true;
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

bool SqlExec(const QString& dbPath, const QString& sql)
{
    const QString conn = QString("local_store_exec_%1").arg(++g_conn_seq);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(dbPath);
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec(sql);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

QString NextClientId(const char* prefix)
{
    return QString("%1-%2").arg(QLatin1String(prefix)).arg(++g_client_seq);
}

//本端发出的文本消息（server_message_id 未确认 = 0）
LocalMessageDTO MakeOutgoingText(qint64 threadId)
{
    LocalMessageDTO dto;
    dto.client_message_id = NextClientId("txt");
    dto.thread_id = threadId;
    dto.sender_id = kSelf;
    dto.receiver_id = kPeer;
    dto.message_type = 0;
    dto.content = QString("hello %1").arg(dto.client_message_id);
    dto.content_size = QString::number(dto.content.toUtf8().size());
    return dto;
}

//对端发来的已分配服务端 id 的消息
LocalMessageDTO MakeIncoming(qint64 threadId, qint64 serverMessageId)
{
    LocalMessageDTO dto;
    dto.client_message_id = NextClientId("inc");
    dto.server_message_id = serverMessageId;
    dto.thread_id = threadId;
    dto.sender_id = kPeer;
    dto.receiver_id = kSelf;
    dto.message_type = 0;
    dto.content = QString("incoming %1").arg(serverMessageId);
    dto.content_size = QString::number(dto.content.toUtf8().size());
    dto.created_at = QStringLiteral("2026-08-13T10:00:00");
    return dto;
}

// ---------------------------------------------------------------------------
// Group 1 — open() 建库：六表存在、WAL 生效、sync_state 初始化为 (0,false)。
// ---------------------------------------------------------------------------
void Group1_OpenSchemaWal()
{
    std::printf("\n== Group 1: open / schema / WAL ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    CheckDetail(db.open(dbPath, kSelf), "open() succeeds", dbPath);
    Check(db.isOpen(), "isOpen() true after open");

    //WAL 持久化在库文件头里，独立连接回读仍为 wal
    QString mode;
    CheckDetail(SqlScalar(dbPath, "PRAGMA journal_mode;", &mode)
                && mode.compare(QLatin1String("wal"), Qt::CaseInsensitive) == 0,
                "journal_mode is WAL", mode);

    QString tableCount;
    CheckDetail(SqlScalar(dbPath,
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name IN"
				" ('messages','conversations','outbox','sync_state','friend_requests','contacts');", &tableCount)
				&& tableCount == QLatin1String("6"),
				"six unified local tables exist", tableCount);

    qint64 seq = -1;
    bool bootstrap = true;
    Check(db.getSyncState(&seq, &bootstrap), "getSyncState on fresh db");
    Check(seq == 0 && !bootstrap, "fresh sync_state is (0, false)");
}

// ---------------------------------------------------------------------------
// Group 2 — 文本发送链路：enqueueSend 单事务写 messages(sending)+outbox，
//           confirmTextSent 回写 server id、置 sent、删 outbox。
// ---------------------------------------------------------------------------
void Group2_TextSendChain()
{
    std::printf("\n== Group 2: enqueueSend / confirmTextSent ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    LocalMessageDTO dto = MakeOutgoingText(kThread);
    const QString cid = dto.client_message_id;
    OutboxEntryDTO committed;
    Check(db.enqueueSend(dto, &committed), "enqueueSend returns true");
    Check(dto.local_id > 0, "enqueueSend back-fills local_id");
    Check(dto.send_state == SEND_STATE_SENDING, "dto send_state forced to sending");
    Check(!dto.created_at.isEmpty(), "created_at auto-filled");

    //CommittedSend：出参条目就是本事务刚插入的 outbox 行（Dispatcher 增量登记的事实载荷）
    Check(committed.operation_id > 0, "committed entry carries operation_id");
    Check(committed.operation_type == OUTBOX_OP_SEND_TEXT, "committed op is SEND_TEXT");
    Check(committed.dedup_key == cid && committed.request_id == cid,
          "committed dedup_key/request_id = client_message_id");
    Check(committed.payload.contains(cid), "committed payload carries unique_id");
    Check(committed.stage.isEmpty(), "committed text entry has no stage");

    LocalMessageDTO stored;
    Check(db.getMessageByClientId(cid, &stored), "getMessageByClientId after enqueue");
    Check(stored.send_state == SEND_STATE_SENDING, "stored row send_state=sending");
    Check(stored.server_message_id == 0, "server_message_id stays 0 (NULL) before confirm");

    QList<OutboxEntryDTO> outbox;
    Check(db.loadOutbox(&outbox) && outbox.size() == 1, "one outbox entry after enqueue");
    if (outbox.size() == 1) {
        const OutboxEntryDTO& e = outbox.first();
        Check(e.operation_type == OUTBOX_OP_SEND_TEXT, "outbox op is SEND_TEXT");
        Check(e.dedup_key == cid && e.request_id == cid,
              "outbox dedup_key/request_id = client_message_id");
        Check(e.payload == committed.payload,
              "outbox row payload equals committed entry payload");
        Check(e.payload.contains(QLatin1String("\"content\"")),
              "outbox payload carries text content");
        Check(e.payload.contains(cid), "outbox payload carries unique_id");
        Check(e.stage.isEmpty(), "text outbox entry has no stage");
    }

    const qint64 kSid = 50001;
    LocalMessageDTO confirmed;
    Check(db.confirmTextSent(cid, kSid, QStringLiteral("2026-08-13T10:01:00"), &confirmed),
          "confirmTextSent returns true");
    Check(confirmed.server_message_id == kSid, "confirm writes server_message_id");
    Check(confirmed.send_state == SEND_STATE_SENT, "confirm sets send_state=sent");
    Check(confirmed.created_at == QLatin1String("2026-08-13T10:01:00"),
          "confirm stores chat_time");
    Check(db.loadOutbox(&outbox) && outbox.isEmpty(), "outbox entry deleted after confirm");
}

// ---------------------------------------------------------------------------
// Group 3 — 崩溃恢复：enqueue 后不 confirm 直接 close，重开能 loadOutbox
//           恢复该 SEND_TEXT；confirm 后重开 outbox 为空。
//           每次重开用新 LocalChatDb 实例（新连接名），规避 Qt 连接告警。
// ---------------------------------------------------------------------------
void Group3_CrashRecovery()
{
    std::printf("\n== Group 3: crash recovery via outbox ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    QString cid;
    {
        LocalChatDb db;
        Check(db.open(dbPath, kSelf), "open() succeeds");
        LocalMessageDTO dto = MakeOutgoingText(kThread);
        cid = dto.client_message_id;
        Check(db.enqueueSend(dto), "enqueue before simulated crash");
        db.close();
        Check(!db.isOpen(), "close() reports closed");
        //不 confirm 直接析构 = 模拟进程崩溃
    }

    {
        LocalChatDb db2;
        Check(db2.open(dbPath, kSelf), "reopen after simulated crash");
        QList<OutboxEntryDTO> outbox;
        Check(db2.loadOutbox(&outbox) && outbox.size() == 1,
              "SEND_TEXT entry recovered from outbox after reopen");
        if (outbox.size() == 1) {
            const OutboxEntryDTO& e = outbox.first();
            Check(e.operation_type == OUTBOX_OP_SEND_TEXT && e.dedup_key == cid,
                  "recovered entry matches the crashed send");
            Check(e.payload.contains(cid), "recovered payload intact for resend");
        }
        LocalMessageDTO confirmed;
        Check(db2.confirmTextSent(cid, 60001, QStringLiteral("2026-08-13T10:05:00"), &confirmed),
              "confirm works after reopen");
        db2.close();
    }

    {
        LocalChatDb db3;
        Check(db3.open(dbPath, kSelf), "second reopen");
        QList<OutboxEntryDTO> outbox;
        Check(db3.loadOutbox(&outbox) && outbox.isEmpty(),
              "outbox empty after confirmed send + reopen");
    }
}

// ---------------------------------------------------------------------------
// Group 4 — 资源链路：updateResourceStage(1504) 只推进 outbox.stage 不删条目，
//           confirmResourceSent(1506 resource_status=Ready) 才置 sent 并删 outbox。
// ---------------------------------------------------------------------------
void Group4_ImageChain()
{
    std::printf("\n== Group 4: resource send chain (1504/1506) ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    //真实源文件存在性由 dispatcher 侧校验；本地库只持久化路径与哈希
    const QString imagePath = dir.path() + "/pic.png";
    {
        QFile file(imagePath);
        Check(file.open(QIODevice::WriteOnly), "temp image file created");
        file.write("fake-png-bytes");
        file.close();
    }

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    LocalMessageDTO dto;
    dto.client_message_id = NextClientId("img");
    dto.thread_id = kThread;
    dto.sender_id = kSelf;
    dto.receiver_id = kPeer;
    dto.message_type = 1;
    dto.content = QStringLiteral("pic.png");        //content = 原始文件名（仅展示）
    dto.local_path = imagePath;
    dto.content_size = QStringLiteral("15");
    dto.resource_status = 0;                        //RESOURCE_UPLOADING
    dto.content_hash = QStringLiteral("f32b67c7c2631af8f3c4b9d41c71ef0c3b8a4c6a1d2e3f4051627384950a1b2c");
    dto.mime_type = QStringLiteral("image/png");
    const QString cid = dto.client_message_id;

    OutboxEntryDTO committed_res;
    Check(db.enqueueSend(dto, &committed_res), "enqueue resource returns true");
    Check(committed_res.operation_id > 0
          && committed_res.operation_type == OUTBOX_OP_SEND_RESOURCE
          && committed_res.stage == RESOURCE_STAGE_METADATA
          && committed_res.dedup_key == cid,
          "committed resource entry matches the persisted row");
    QList<OutboxEntryDTO> outbox;
    Check(db.loadOutbox(&outbox) && outbox.size() == 1, "resource outbox entry created");
    if (outbox.size() == 1) {
        const OutboxEntryDTO& e = outbox.first();
        Check(e.operation_type == OUTBOX_OP_SEND_RESOURCE, "outbox op is SEND_RESOURCE");
        Check(e.stage == RESOURCE_STAGE_METADATA, "initial stage is metadata");
        Check(e.payload.contains(QLatin1String("\"file_name\"")),
              "resource payload carries file_name");
        Check(e.payload.contains(QLatin1String("\"content_hash\"")),
              "resource payload carries content_hash");
        Check(e.payload.contains(QLatin1String("image/png")),
              "resource payload carries mime_type");
        Check(!e.payload.contains(QLatin1String("\"md5\"")),
              "legacy md5 field is gone");
    }

    const qint64 kSid = 70001;
    LocalMessageDTO staged;
    Check(db.updateResourceStage(cid, kSid, RESOURCE_STAGE_UPLOADING, &staged),
          "updateResourceStage returns true");
    Check(staged.server_message_id == kSid, "1504 writes server_message_id");
    Check(staged.send_state == SEND_STATE_SENDING, "still sending after 1504");
    Check(db.loadOutbox(&outbox) && outbox.size() == 1
          && outbox.first().stage == RESOURCE_STAGE_UPLOADING,
          "updateResourceStage advances stage without deleting outbox entry");

    LocalMessageDTO done;
    Check(db.confirmResourceSent(cid, &done), "confirmResourceSent returns true");
    Check(done.send_state == SEND_STATE_SENT, "send_state=sent after 1506");
    Check(done.server_message_id == kSid, "server_message_id kept after 1506");
    Check(db.loadOutbox(&outbox) && outbox.isEmpty(),
          "confirmResourceSent deletes the outbox entry");
}

// ---------------------------------------------------------------------------
// Group 5 — markSendFailed：send_state=failed 且 outbox 删除。
// ---------------------------------------------------------------------------
void Group5_MarkSendFailed()
{
    std::printf("\n== Group 5: markSendFailed ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    LocalMessageDTO dto = MakeOutgoingText(kThread);
    const QString cid = dto.client_message_id;
    Check(db.enqueueSend(dto), "enqueue before failure");

    LocalMessageDTO failed;
    Check(db.markSendFailed(cid, &failed), "markSendFailed returns true");
    Check(failed.send_state == SEND_STATE_FAILED, "send_state=failed");

    LocalMessageDTO stored;
    Check(db.getMessageByClientId(cid, &stored)
          && stored.send_state == SEND_STATE_FAILED,
          "failed state persisted");
    QList<OutboxEntryDTO> outbox;
    Check(db.loadOutbox(&outbox) && outbox.isEmpty(), "outbox entry deleted on failure");
}

// ---------------------------------------------------------------------------
// Group 6 — insertIncoming 幂等：重复 server_message_id 被 INSERT OR IGNORE
//           吞掉（insertedIds 第二次为空，供 UI 去重），未读只加一次，
//           且不生成接收方 ACK outbox。
// ---------------------------------------------------------------------------
void Group6_InsertIncomingDedup()
{
	std::printf("\n== Group 6: insertIncoming dedup / no ACK ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    const qint64 kSid = 80001;
    QList<LocalMessageDTO> msgs;
    msgs.append(MakeIncoming(kThread, kSid));

    QList<qint64> inserted;
    Check(db.insertIncoming(msgs, &inserted), "first insertIncoming returns true");
    Check(inserted.size() == 1 && inserted.first() == kSid,
          "insertedIds reports the new server id");

    QList<OutboxEntryDTO> outbox;
	Check(db.loadOutbox(&outbox) && outbox.isEmpty(), "incoming message creates no ACK outbox");

    QList<LocalConversationDTO> convs;
    Check(db.loadConversations(&convs) && convs.size() == 1, "conversation upserted");
    if (convs.size() == 1) {
        Check(convs.first().unread_count == 1, "unread_count incremented");
        Check(convs.first().last_server_message_id == kSid,
              "conversation last_server_message_id updated");
    }

    Check(db.insertIncoming(msgs, &inserted), "second insertIncoming returns true (idempotent)");
    Check(inserted.isEmpty(), "duplicate server_message_id swallowed (insertedIds empty)");
	Check(db.loadOutbox(&outbox) && outbox.isEmpty(), "duplicate still creates no ACK");
    Check(db.loadConversations(&convs) && convs.size() == 1
          && convs.first().unread_count == 1,
          "unread_count not bumped by duplicate");
}

// ---------------------------------------------------------------------------
// Group 7 — applySyncPage：整页写入与游标推进同事务；在 close 后的 DB 上
//           调用返回 false 且游标保持不变。
// ---------------------------------------------------------------------------
void Group7_ApplySyncPage()
{
    std::printf("\n== Group 7: applySyncPage atomicity / failure path ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    QList<LocalMessageDTO> page;
    page.append(MakeIncoming(kThread, 90001));
    page.append(MakeIncoming(kThread, 90002));

    QList<qint64> inserted;
    Check(db.applySyncPage(page, 90002, &inserted), "applySyncPage returns true");
    Check(inserted.size() == 2, "whole page inserted");

    qint64 seq = 0;
    bool bootstrap = true;
    Check(db.getSyncState(&seq, &bootstrap) && seq == 90002,
          "sync cursor advanced to newSyncSeq");
    Check(!bootstrap, "bootstrap flag untouched by applySyncPage");

    QList<LocalMessageDTO> loaded;
    Check(db.loadRecentMessages(kThread, 10, &loaded) && loaded.size() == 2,
          "page rows readable after apply");

    //失败路径：关闭的 DB 上调用，isOpen() 守卫直接返回 false
    db.close();
    QList<qint64> discarded;
    Check(!db.applySyncPage(page, 99999, &discarded),
          "applySyncPage on closed db returns false");

    //重开后游标必须仍是旧值（失败不动游标）
    LocalChatDb db2;
    Check(db2.open(dbPath, kSelf), "reopen for cursor check");
    qint64 seq2 = -1;
    Check(db2.getSyncState(&seq2, NULL) && seq2 == 90002,
          "cursor unchanged after failed applySyncPage");
}

// ---------------------------------------------------------------------------
// Group 8 — 64 位往返：>2^32 的 server_message_id / recv_seq 写入读出无损。
//           覆盖 INSERT（applySyncPage）与 UPDATE（confirmTextSent）两条路径。
// ---------------------------------------------------------------------------
void Group8_BigIntRoundTrip()
{
    std::printf("\n== Group 8: 64-bit id round-trip (>2^32) ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    const qint64 kBigSid = 4294967300LL;  // 2^32 + 4
    const qint64 kBigSeq = 4294967301LL;
    const qint64 kBigSid2 = 4294967302LL;

    QList<LocalMessageDTO> page;
    page.append(MakeIncoming(kThread, kBigSid));
    QList<qint64> inserted;
    Check(db.applySyncPage(page, kBigSeq, &inserted), "applySyncPage with >2^32 values");
    Check(inserted.size() == 1 && inserted.first() == kBigSid,
          "64-bit server id in insertedIds");

    qint64 seq = 0;
    Check(db.getSyncState(&seq, NULL) && seq == kBigSeq, "64-bit recv_seq round-trip");

    QList<LocalMessageDTO> loaded;
    Check(db.loadRecentMessages(kThread, 10, &loaded) && loaded.size() == 1
          && loaded.first().server_message_id == kBigSid,
          "64-bit server_message_id round-trip (INSERT path)");

    QList<LocalConversationDTO> convs;
    Check(db.loadConversations(&convs) && convs.size() == 1
          && convs.first().last_server_message_id == kBigSid,
          "64-bit last_server_message_id in conversation");

    //UPDATE 路径：confirmTextSent 绑定 64 位 server_message_id
    LocalMessageDTO dto = MakeOutgoingText(kThread);
    const QString cid = dto.client_message_id;
    Check(db.enqueueSend(dto), "enqueue for 64-bit confirm");
    LocalMessageDTO confirmed;
    Check(db.confirmTextSent(cid, kBigSid2, QStringLiteral("2026-08-13T11:00:00"), &confirmed)
          && confirmed.server_message_id == kBigSid2,
          "64-bit server_message_id round-trip (UPDATE path)");
}

// ---------------------------------------------------------------------------
// Group 9 — loadRecentMessages / loadOlderMessages 的分页与排序：
//           recent 取最新 N 条按 local_id 升序返回；older 取游标之前 N 条
//           按 server_message_id 升序返回；thread 之间互不可见。
// ---------------------------------------------------------------------------
void Group9_PaginationOrdering()
{
    std::printf("\n== Group 9: loadRecent/loadOlder pagination & ordering ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    const qint64 kThreadB = 200;
    QList<LocalMessageDTO> msgs;
    for (qint64 sid = 101; sid <= 105; ++sid) {
        msgs.append(MakeIncoming(kThread, sid));
    }
    msgs.append(MakeIncoming(kThreadB, 201));  //另一个会话的干扰项
    QList<qint64> inserted;
    Check(db.insertIncoming(msgs, &inserted), "seed messages inserted");
    Check(inserted.size() == 6, "all seeds actually inserted");

    QList<LocalMessageDTO> recent;
    Check(db.loadRecentMessages(kThread, 2, &recent) && recent.size() == 2,
          "loadRecentMessages honors limit");
    Check(recent.size() == 2
          && recent.first().server_message_id == 104
          && recent.last().server_message_id == 105,
          "recent page = newest N, ascending order");

    Check(db.loadRecentMessages(kThread, 10, &recent) && recent.size() == 5,
          "other thread's messages are invisible");

    QList<LocalMessageDTO> older;
    Check(db.loadOlderMessages(kThread, 104, 2, &older) && older.size() == 2,
          "loadOlderMessages honors limit");
    Check(older.size() == 2
          && older.first().server_message_id == 102
          && older.last().server_message_id == 103,
          "older page = rows below cursor, ascending order");

    Check(db.loadOlderMessages(kThread, 102, 10, &older) && older.size() == 1
          && older.first().server_message_id == 101,
          "older paging stops at the oldest row");
}

// ---------------------------------------------------------------------------
// Group 10 — 同步页不创建 ACK；只原子写业务数据并推进 recv_seq。
// ---------------------------------------------------------------------------
void Group10_SyncPageNoAck()
{
	std::printf("\n== Group 10: applySyncPage without ACK ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    //一页混合两条消息，二者都只写业务数据和推进游标。
    QList<LocalMessageDTO> page;
    page.append(MakeIncoming(kThread, 91001));
    {
        LocalMessageDTO self_msg = MakeIncoming(kThread, 91002);
        self_msg.sender_id = kSelf;
        self_msg.receiver_id = kPeer;
        page.append(self_msg);
    }

    QList<qint64> inserted;
    Check(db.applySyncPage(page, 91002, &inserted), "applySyncPage returns true");
    Check(inserted.size() == 2, "whole page inserted");

    QList<OutboxEntryDTO> outbox;
	Check(db.loadOutbox(&outbox) && outbox.isEmpty(), "sync page creates no ACK outbox");

    //重复同步同一页：INSERT OR IGNORE 吞掉，不产生额外业务数据。
    Check(db.applySyncPage(page, 91002, &inserted), "re-applying the same page returns true");
    Check(inserted.isEmpty(), "duplicate page rows swallowed");
	Check(db.loadOutbox(&outbox) && outbox.isEmpty(), "duplicate page creates no ACK");
}

// ---------------------------------------------------------------------------
// Group 11 — FILE_MSG（msg_type=3）发送链路：与图片同走 SEND_RESOURCE outbox
//           状态机；payload 携带 file_name/content_hash/mime_type。
// ---------------------------------------------------------------------------
void Group11_FileMsgChain()
{
    std::printf("\n== Group 11: FILE_MSG resource chain ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    const QString filePath = dir.path() + "/report.pdf";
    {
        QFile file(filePath);
        Check(file.open(QIODevice::WriteOnly), "temp file created");
        file.write("fake-pdf-bytes");
        file.close();
    }

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    LocalMessageDTO dto;
    dto.client_message_id = NextClientId("file");
    dto.thread_id = kThread;
    dto.sender_id = kSelf;
    dto.receiver_id = kPeer;
    dto.message_type = 3;                            //ChatMsgType::FILE
    dto.content = QStringLiteral("report.pdf");
    dto.local_path = filePath;
    dto.content_size = QStringLiteral("15");
    dto.resource_status = 0;
    dto.content_hash = QString(64, QLatin1Char('a'));
    dto.mime_type = QStringLiteral("application/pdf");
    const QString cid = dto.client_message_id;

    Check(db.enqueueSend(dto), "enqueue file returns true");
    QList<OutboxEntryDTO> outbox;
    Check(db.loadOutbox(&outbox) && outbox.size() == 1, "file outbox entry created");
    if (outbox.size() == 1) {
        const OutboxEntryDTO& e = outbox.first();
        Check(e.operation_type == OUTBOX_OP_SEND_RESOURCE, "file uses SEND_RESOURCE");
        Check(e.stage == RESOURCE_STAGE_METADATA, "initial stage is metadata");
        Check(e.payload.contains(QLatin1String("application/pdf")),
              "file payload carries mime_type");
    }

    LocalMessageDTO staged;
    Check(db.updateResourceStage(cid, 81001, RESOURCE_STAGE_UPLOADING, &staged),
          "file updateResourceStage works");
    LocalMessageDTO done;
    Check(db.confirmResourceSent(cid, &done)
          && done.send_state == SEND_STATE_SENT,
          "file confirmResourceSent works");
    Check(db.loadOutbox(&outbox) && outbox.isEmpty(), "file outbox cleaned");

    //本地行保留资源字段（打开会话页时据此渲染 FileBubble）
    LocalMessageDTO stored;
    Check(db.getMessageByClientId(cid, &stored)
          && stored.message_type == 3
          && stored.mime_type == QLatin1String("application/pdf"),
          "stored row keeps msg_type/mime_type");
}

void Group12_FriendMessageState()
{
    std::printf("\n== Group 12: friend messages / contacts ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    LocalMessageDTO apply;
    apply.server_message_id = 92001;
    apply.client_message_id = QStringLiteral("friend-apply-92001");
    apply.sender_id = kSelf;
    apply.receiver_id = kPeer;
    apply.message_type = 10;
    apply.business_status = 1;
    apply.content = QStringLiteral("hello");
    apply.requester_remark = QStringLiteral("peer remark");
    apply.sender_name = QStringLiteral("self");
    apply.created_at = QStringLiteral("2026-08-26T10:00:00");
    QList<qint64> friendInserted;
    Check(db.insertIncoming(QList<LocalMessageDTO>() << apply, &friendInserted),
          "outgoing application response persisted");
	Check(friendInserted.size() == 1 && friendInserted.first() == 92001,
		"new friend application reports its canonical id");
	Check(db.insertIncoming(QList<LocalMessageDTO>() << apply, &friendInserted) &&
		friendInserted.isEmpty(), "duplicate friend application is swallowed");

    QString value;
    Check(SqlScalar(dbPath,
          "SELECT business_status FROM friend_requests WHERE message_id=92001", &value)
          && value == QLatin1String("1"), "application is pending");
	LocalMessageDTO reverseApply = apply;
	reverseApply.server_message_id = 92000;
	reverseApply.client_message_id = QStringLiteral("friend-apply-92000");
	reverseApply.sender_id = kPeer;
	reverseApply.receiver_id = kSelf;
	Check(db.insertIncoming(QList<LocalMessageDTO>() << reverseApply),
		"opposite-direction application persisted");

    LocalMessageDTO accept;
    accept.server_message_id = 92002;
    accept.recv_seq = 1;
    accept.client_message_id = QStringLiteral("server-92002");
    accept.thread_id = 45001;
    accept.sender_id = kPeer;
    accept.receiver_id = kSelf;
    accept.message_type = 11;
    accept.business_status = 2;
    accept.related_message_id = 92001;
    accept.content = QStringLiteral("We are friends now!");
    accept.sender_name = QStringLiteral("peer");
    accept.sender_nick = QStringLiteral("Peer");
    accept.requester_remark = QStringLiteral("peer remark");
    accept.created_at = QStringLiteral("2026-08-26T10:01:00");
    accept.handled_at = accept.created_at;
    Check(db.applySyncPage(QList<LocalMessageDTO>() << accept, 1),
          "accept result applied");
    Check(SqlScalar(dbPath,
          "SELECT business_status FROM friend_requests WHERE message_id=92001", &value)
          && value == QLatin1String("2"), "application becomes accepted");
	Check(SqlScalar(dbPath,
		  "SELECT business_status FROM friend_requests WHERE message_id=92000", &value)
		  && value == QLatin1String("2"),
		  "accept closes the opposite-direction pending application");
    Check(SqlScalar(dbPath, "SELECT COUNT(*) FROM contacts WHERE uid=2002", &value)
          && value == QLatin1String("1"), "accept creates contact");
    Check(SqlScalar(dbPath, "SELECT COUNT(*) FROM messages WHERE message_type=11", &value)
          && value == QLatin1String("1"), "accept creates one system message");

    LocalMessageDTO apply2 = apply;
    apply2.server_message_id = 92003;
    apply2.client_message_id = QStringLiteral("friend-apply-92003");
    Check(db.insertIncoming(QList<LocalMessageDTO>() << apply2),
          "second outgoing application persisted");
    LocalMessageDTO reject;
    reject.server_message_id = 92004;
    reject.recv_seq = 2;
    reject.client_message_id = QStringLiteral("server-92004");
    reject.sender_id = kPeer;
    reject.receiver_id = kSelf;
    reject.message_type = 12;
    reject.business_status = 3;
    reject.related_message_id = 92003;
    reject.content = QStringLiteral("not now");
    reject.created_at = QStringLiteral("2026-08-26T10:02:00");
    reject.handled_at = reject.created_at;
    Check(db.applySyncPage(QList<LocalMessageDTO>() << reject, 2),
          "reject result applied");
    Check(SqlScalar(dbPath,
          "SELECT business_status FROM friend_requests WHERE message_id=92003", &value)
          && value == QLatin1String("3"), "application becomes rejected");
    Check(SqlScalar(dbPath, "SELECT COUNT(*) FROM contacts", &value)
          && value == QLatin1String("1"), "reject creates no contact");
    Check(SqlScalar(dbPath, "SELECT COUNT(*) FROM messages WHERE message_type=12", &value)
          && value == QLatin1String("0"), "reject is not inserted into chat history");
    qint64 seq = 0;
    Check(db.getSyncState(&seq, NULL) && seq == 2,
          "friend results advance recv cursor");
    QList<OutboxEntryDTO> outbox;
    Check(db.loadOutbox(&outbox) && outbox.isEmpty(),
          "friend results create no ACK outbox");

    LocalMessageDTO reconcileAccept = apply;
    reconcileAccept.server_message_id = 92005;
    reconcileAccept.client_message_id = QStringLiteral("friend-apply-92005");
    LocalMessageDTO reconcileReject = apply;
    reconcileReject.server_message_id = 92006;
    reconcileReject.client_message_id = QStringLiteral("friend-apply-92006");
    reconcileReject.receiver_id = 2003;
    LocalMessageDTO stillPending = apply;
    stillPending.server_message_id = 92007;
    stillPending.client_message_id = QStringLiteral("friend-apply-92007");
    stillPending.sender_id = 2004;
    stillPending.receiver_id = kSelf;
    Check(db.insertIncoming(QList<LocalMessageDTO>()
          << reconcileAccept << reconcileReject << stillPending),
          "pending applications seeded for reconnect reconciliation");

    QJsonObject pendingSnapshot;
    pendingSnapshot["message_id"] = QStringLiteral("92007");
    pendingSnapshot["from_uid"] = 2004;
    pendingSnapshot["to_uid"] = static_cast<int>(kSelf);
    pendingSnapshot["status"] = 1;
    pendingSnapshot["desc"] = QStringLiteral("still pending");
    QJsonObject contactSnapshot;
    contactSnapshot["uid"] = static_cast<int>(kPeer);
    contactSnapshot["thread_id"] = QStringLiteral("45001");
    contactSnapshot["name"] = QStringLiteral("peer");
    contactSnapshot["back"] = QStringLiteral("peer remark");
    QJsonArray pendingSnapshots;
    pendingSnapshots.append(pendingSnapshot);
    QJsonArray contactSnapshots;
    contactSnapshots.append(contactSnapshot);
    Check(db.applySnapshot(pendingSnapshots, contactSnapshots, false),
          "reconnect snapshot merges and reconciles pending applications");
    Check(SqlScalar(dbPath,
          "SELECT business_status FROM friend_requests WHERE message_id=92005", &value)
          && value == QLatin1String("2"),
          "missing pending request with contact reconciles to accepted");
    Check(SqlScalar(dbPath,
          "SELECT related_thread_id FROM friend_requests WHERE message_id=92005", &value)
          && value == QLatin1String("45001"),
          "accepted reconciliation records contact thread");
    Check(SqlScalar(dbPath,
          "SELECT business_status FROM friend_requests WHERE message_id=92006", &value)
          && value == QLatin1String("3"),
          "missing pending request without contact reconciles to rejected");
    Check(SqlScalar(dbPath,
          "SELECT business_status FROM friend_requests WHERE message_id=92007", &value)
          && value == QLatin1String("1"),
          "request present in snapshot remains pending");
}

void Group13_FriendCursorRollback()
{
    std::printf("\n== Group 13: friend transaction rollback ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");
    LocalMessageDTO apply;
    apply.server_message_id = 93001;
    apply.client_message_id = QStringLiteral("friend-apply-93001");
    apply.sender_id = kSelf;
    apply.receiver_id = kPeer;
    apply.message_type = 10;
    apply.business_status = 1;
    Check(db.insertIncoming(QList<LocalMessageDTO>() << apply),
          "pending application seeded");
    Check(SqlExec(dbPath, "DROP TABLE contacts"),
          "contact table removed to force SQL failure");

    LocalMessageDTO accept;
    accept.server_message_id = 93002;
    accept.recv_seq = 1;
    accept.client_message_id = QStringLiteral("server-93002");
    accept.thread_id = 46001;
    accept.sender_id = kPeer;
    accept.receiver_id = kSelf;
    accept.message_type = 11;
    accept.business_status = 2;
    accept.related_message_id = 93001;
    Check(!db.applySyncPage(QList<LocalMessageDTO>() << accept, 1),
          "accept page fails when business write fails");
    qint64 seq = -1;
    Check(db.getSyncState(&seq, NULL) && seq == 0,
          "cursor rolls back with failed business write");
    QString value;
    Check(SqlScalar(dbPath,
          "SELECT business_status FROM friend_requests WHERE message_id=93001", &value)
          && value == QLatin1String("1"), "friend status also rolls back");
    Check(SqlScalar(dbPath, "SELECT COUNT(*) FROM messages", &value)
          && value == QLatin1String("0"), "system message also rolls back");
}

void Group14_SchemaVersionRebuild()
{
    std::printf("\n== Group 14: destructive schema rebuild ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";
    {
        LocalChatDb db;
        Check(db.open(dbPath, kSelf), "open() succeeds");
        LocalMessageDTO dto = MakeOutgoingText(kThread);
        Check(db.enqueueSend(dto), "old schema data seeded");
    }
    Check(SqlExec(dbPath, "PRAGMA user_version=1"),
          "schema version changed to obsolete value");
    {
        LocalChatDb rebuilt;
        Check(rebuilt.open(dbPath, kSelf), "open rebuilds obsolete schema");
        QString value;
        Check(SqlScalar(dbPath, "SELECT COUNT(*) FROM messages", &value)
              && value == QLatin1String("0"), "old local messages discarded");
        Check(SqlScalar(dbPath, "PRAGMA user_version", &value)
              && value == QLatin1String("3"), "schema version advanced to 3");
        QList<OutboxEntryDTO> outbox;
        Check(rebuilt.loadOutbox(&outbox) && outbox.isEmpty(),
              "obsolete outbox discarded with schema");
    }
}

} // namespace

int main(int argc, char** argv)
{
    //LocalChatDb 全同步、非 QObject，无需事件循环；QCoreApplication 仅为
    //Qt 与静态 QSQLITE 插件的正确初始化。
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);

    std::printf("=== local_store_tests ===");

    Group1_OpenSchemaWal();
    Group2_TextSendChain();
    Group3_CrashRecovery();
    Group4_ImageChain();
    Group5_MarkSendFailed();
    Group6_InsertIncomingDedup();
    Group7_ApplySyncPage();
    Group8_BigIntRoundTrip();
    Group9_PaginationOrdering();
	Group10_SyncPageNoAck();
    Group11_FileMsgChain();
    Group12_FriendMessageState();
    Group13_FriendCursorRollback();
    Group14_SchemaVersionRebuild();

    std::printf("\n=== summary: %d failure(s) ===\n", g_failures);
    if (g_failures != 0) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}
