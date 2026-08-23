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
// Group 1 — open() 建库：四表存在、WAL 生效、sync_state 初始化为 (0,false)。
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
                " ('messages','conversations','outbox','sync_state');", &tableCount)
                && tableCount == QLatin1String("4"),
                "four tables exist (messages/conversations/outbox/sync_state)", tableCount);

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
    Check(db.enqueueSend(dto), "enqueueSend returns true");
    Check(dto.local_id > 0, "enqueueSend back-fills local_id");
    Check(dto.send_state == SEND_STATE_SENDING, "dto send_state forced to sending");
    Check(!dto.created_at.isEmpty(), "created_at auto-filled");

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
//           confirmResourceSent(1508 resource_status=Ready) 才置 sent 并删 outbox。
// ---------------------------------------------------------------------------
void Group4_ImageChain()
{
    std::printf("\n== Group 4: resource send chain (1504/1508) ==\n");
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

    Check(db.enqueueSend(dto), "enqueue resource returns true");
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
    Check(done.send_state == SEND_STATE_SENT, "send_state=sent after 1508");
    Check(done.server_message_id == kSid, "server_message_id kept after 1508");
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
//           且仅生成一条 DELIVERY_ACK outbox。
// ---------------------------------------------------------------------------
void Group6_InsertIncomingDedup()
{
    std::printf("\n== Group 6: insertIncoming dedup / DELIVERY_ACK ==\n");
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
    Check(db.loadOutbox(&outbox) && outbox.size() == 1, "DELIVERY_ACK outbox entry created");
    if (outbox.size() == 1) {
        const OutboxEntryDTO& e = outbox.first();
        Check(e.operation_type == OUTBOX_OP_DELIVERY_ACK, "outbox op is DELIVERY_ACK");
        Check(e.dedup_key == QString("ack_%1").arg(kSid), "ack dedup_key is ack_<server_id>");
        Check(e.payload.contains(QString::number(kSid)), "ack payload carries message_id");
    }

    QList<LocalConversationDTO> convs;
    Check(db.loadConversations(&convs) && convs.size() == 1, "conversation upserted");
    if (convs.size() == 1) {
        Check(convs.first().unread_count == 1, "unread_count incremented");
        Check(convs.first().last_server_message_id == kSid,
              "conversation last_server_message_id updated");
    }

    Check(db.insertIncoming(msgs, &inserted), "second insertIncoming returns true (idempotent)");
    Check(inserted.isEmpty(), "duplicate server_message_id swallowed (insertedIds empty)");
    Check(db.loadOutbox(&outbox) && outbox.size() == 1,
          "no duplicate ACK for duplicate message");
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
// Group 8 — 64 位往返：>2^32 的 server_message_id / sync_seq 写入读出无损。
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
    Check(db.getSyncState(&seq, NULL) && seq == kBigSeq, "64-bit sync_seq round-trip");

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
// Group 10 — 同步页 ACK 闭环：applySyncPage 对实际新插入的“他人消息”在同一
//           事务生成 DELIVERY_ACK（与 insertIncoming 对齐）；自己发的消息与
//           重复页不产生 ACK。这是离线资源消息不再依赖服务端重推的关键。
// ---------------------------------------------------------------------------
void Group10_SyncPageAck()
{
    std::printf("\n== Group 10: applySyncPage DELIVERY_ACK closure ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const QString dbPath = dir.path() + "/chat.db";

    LocalChatDb db;
    Check(db.open(dbPath, kSelf), "open() succeeds");

    //一页混合：他人消息 91001（产生 ACK）+ 自己消息 91002（不产生 ACK）
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
    Check(db.loadOutbox(&outbox) && outbox.size() == 1,
          "exactly one DELIVERY_ACK created for the received message");
    if (outbox.size() == 1) {
        const OutboxEntryDTO& e = outbox.first();
        Check(e.operation_type == OUTBOX_OP_DELIVERY_ACK, "outbox op is DELIVERY_ACK");
        Check(e.dedup_key == QLatin1String("ack_91001"), "ack dedup_key is ack_91001");
    }

    //重复同步同一页：INSERT OR IGNORE 吞掉，不产生第二条 ACK
    Check(db.applySyncPage(page, 91002, &inserted), "re-applying the same page returns true");
    Check(inserted.isEmpty(), "duplicate page rows swallowed");
    Check(db.loadOutbox(&outbox) && outbox.size() == 1,
          "no duplicate ACK from duplicate sync page");
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
    Group10_SyncPageAck();
    Group11_FileMsgChain();

    std::printf("\n=== summary: %d failure(s) ===\n", g_failures);
    if (g_failures != 0) {
        std::printf("RESULT: FAIL\n");
        return 1;
    }
    std::printf("RESULT: PASS\n");
    return 0;
}
