// outbox_dispatcher_tests — OutboxDispatcher 单向状态机（commit 边界）测试。
//
// 验证三条不变式（对应 20260829 outbox 重构）：
//   1. 增量登记：enqueueSend 事务 commit 后 Dispatcher 才登记并首发；
//      不再有每条消息触发的全表 loadOutbox 重建。
//   2. 销账边界：1302/1504/1506 回包只发起本地事务，commit 成功
//      （sig_send_confirmed 等 ok=true）后条目才离开登记簿、才停止重发。
//   3. 恢复语义：全表快照仅登录恢复消费；恢复之外到达的旧快照
//      不得复活已销账条目（ !_restoring 守卫）。
//
// 无测试框架：plain main() 逐断言打印 [PASS]/[FAIL]，任一失败退出码非零
// （与 local_store_tests 同风格）。
//
// 线程模型：测试进程不创建 LocalChatThread/TcpThread，所有单例留在主线程
// → store 的"请求信号→worker→结果信号"链路全部 direct 同步执行，
// 时序完全确定；Dispatcher 的 250ms 扫描经 Pump() 里的事件循环驱动。
// 测试用 emit TcpMgr/LocalChatStore 公有信号直接注入 login_ready/1302/1504，
// 以 TcpMgr::sig_send_data 计数（按消息 ID 过滤）观察网络发送，
// 用第二个 LocalChatDb 连接直查 SQLite 真值（WAL 支持多连接）。
//
// QApplication（而非 QCoreApplication）是必须的：1504 成功路径的
// startResourceUpload 会构造 QPixmap 重建 MsgInfo。

#include "outboxdispatcher.h"
#include "tcpmgr.h"
#include "filetcpmgr.h"
#include "localchatstore.h"
#include "localchatdb.h"
#include "global.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <cstdio>

namespace {

int g_failures = 0;
qint64 g_client_seq = 0;

void Check(bool cond, const char* name)
{
    if (cond) { std::printf("[PASS] %s\n", name); }
    else { std::printf("[FAIL] %s\n", name); ++g_failures; }
}

//驱动事件循环（扫描定时器、queued 信号）前进 ms 毫秒
void Pump(int ms)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!elapsed.hasExpired(ms)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

const qint64 kSelf = 1001;
const qint64 kPeer = 2002;
const qint64 kThread = 100;

QString NextClientId(const char* prefix)
{
    return QString("%1-%2").arg(QLatin1String(prefix)).arg(++g_client_seq);
}

LocalMessageDTO MakeOutgoingText()
{
    LocalMessageDTO dto;
    dto.client_message_id = NextClientId("txt");
    dto.thread_id = kThread;
    dto.sender_id = kSelf;
    dto.receiver_id = kPeer;
    dto.message_type = 0;
    dto.content = QString("hello %1").arg(dto.client_message_id);
    dto.content_size = QString::number(dto.content.toUtf8().size());
    return dto;
}

//openUserDb 在 test-mode AppDataLocation 下的落库路径（与生产同规则）
QString UserDbPath(int uid)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + "/user/" + QString::number(uid) + "/chat.db";
}

//第二个连接直查 SQLite 真值
bool ReadMessageRow(int uid, const QString& cid, LocalMessageDTO* out)
{
    LocalChatDb db;
    if (!db.open(UserDbPath(uid), uid)) {
        return false;
    }
    return db.getMessageByClientId(cid, out);
}

bool OutboxRow(int uid, const QString& cid, OutboxEntryDTO* out)
{
    LocalChatDb db;
    const bool opened = db.open(UserDbPath(uid), uid);
    int total = -1;
    bool loaded = false;
    if (opened) {
        QList<OutboxEntryDTO> entries;
        loaded = db.loadOutbox(&entries);
        total = entries.size();
        for (const OutboxEntryDTO& e : entries) {
            if (e.dedup_key == cid) {
                *out = e;
                return true;
            }
        }
    }
    std::fprintf(stderr, "[DBG] OutboxRow uid=%d open=%d loaded=%d total=%d\n",
        uid, opened ? 1 : 0, loaded ? 1 : 0, total);
    return false;
}

struct SendSpy {
    QHash<int, int> counts;
    int Count(int msg_id) const { return counts.value(msg_id, 0); }
    int Snapshot(int msg_id) const { return Count(msg_id); }
};

//每个 Group 用独立 uid：openUserDb → sig_db_opened → 登记簿整体作废重恢复
void OpenAndLogin(int uid)
{
    LocalChatStore::GetInstance()->openUserDb(uid);
    Pump(30);
    emit TcpMgr::GetInstance()->sig_chat_login_ready();
    Pump(30);
}

} // namespace

// ---------------------------------------------------------------------------
// Group 0 — Chat 登录完成早于 SQLite open：db_opened 到达后必须自行会合并恢复，
//           不能因为先前没有可用数据库而永久停在 _restored=false。
// ---------------------------------------------------------------------------
void Group0_LoginBeforeDbOpen(const SendSpy& spy)
{
    std::printf("\n== Group 0: chat login before local DB open ==\n");
    emit TcpMgr::GetInstance()->sig_chat_login_ready();
    Pump(20);

    const int uid = 41000;
    LocalChatStore::GetInstance()->openUserDb(uid);
    Pump(30);

    const int base = spy.Snapshot(ID_TEXT_CHAT_MSG_REQ);
    LocalMessageDTO dto = MakeOutgoingText();
    const QString cid = dto.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(dto);
    Pump(30);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 1,
          "db_opened completes restore after earlier chat login");

    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::SUCCESS, cid, 990000, QStringLiteral("2026-08-30T10:00:00"));
    Pump(30);
    LocalMessageDTO stored;
    Check(ReadMessageRow(uid, cid, &stored) && stored.send_state == SEND_STATE_SENT,
          "race-case message confirms normally");

    emit TcpMgr::GetInstance()->sig_connection_closed();
    Pump(20);
}

// ---------------------------------------------------------------------------
// Group A — 离线入库不发送；登录恢复后按登记簿首发恰好一次。
// Group B — 1302 成功：本地销账事务 commit 后停止重发，DB=sent 且 outbox 删。
// Group C — 恢复之外的旧全表快照不得复活已销账条目。
// Group D — 销账完成后的重复 1302 被登记簿忽略。
// ---------------------------------------------------------------------------
void GroupAtoD_OfflineRestoreAndAckBoundary(const SendSpy& spy)
{
    std::printf("\n== Group A: offline enqueue + cold restore first send ==\n");
    const int uid = 41001;
    LocalChatStore::GetInstance()->openUserDb(uid);
    Pump(30);
    std::fprintf(stderr, "[DBG] db exists=%d path=%s\n",
        QFile::exists(UserDbPath(uid)) ? 1 : 0, qPrintable(UserDbPath(uid)));

    const int offline_base = spy.Snapshot(ID_TEXT_CHAT_MSG_REQ);
    LocalMessageDTO dto = MakeOutgoingText();
    const QString cid = dto.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(dto);
    Pump(30);
    std::fprintf(stderr, "[DBG] after enqueue spy1301=%d\n", spy.Count(ID_TEXT_CHAT_MSG_REQ));
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == offline_base,
          "offline enqueue registers without sending");

    OutboxEntryDTO row;
    Check(OutboxRow(uid, cid, &row), "outbox row persisted before login");
    std::fprintf(stderr, "[DBG] outbox row found=%d\n", OutboxRow(uid, cid, &row) ? 1 : 0);
    Check(row.operation_type == OUTBOX_OP_SEND_TEXT, "persisted op is SEND_TEXT");

    const int base = spy.Snapshot(ID_TEXT_CHAT_MSG_REQ);
    emit TcpMgr::GetInstance()->sig_chat_login_ready();
    Pump(300);
    std::fprintf(stderr, "[DBG] after login spy1301=%d (base=%d)\n",
        spy.Count(ID_TEXT_CHAT_MSG_REQ), base);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 1,
          "login restore first-sends the entry exactly once");

    std::printf("\n== Group B: 1302 SUCCESS -> confirm commit boundary ==\n");
    const qint64 kSid = 990001;
    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::SUCCESS, cid, kSid, QStringLiteral("2026-08-29T10:00:00"));
    Pump(600); // >= 2 个扫描周期：销账后不得再重发
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 1,
          "no resend after confirm committed");

    LocalMessageDTO stored;
    Check(ReadMessageRow(uid, cid, &stored), "message row readable after confirm");
    Check(stored.send_state == SEND_STATE_SENT, "send_state=sent after 1302");
    Check(stored.server_message_id == kSid, "server_message_id persisted");
    OutboxEntryDTO gone;
    Check(!OutboxRow(uid, cid, &gone), "outbox row deleted after confirm");

    std::printf("\n== Group C: stale full snapshot must not resurrect ==\n");
    OutboxEntryDTO stale;
    stale.operation_id = 999;
    stale.operation_type = OUTBOX_OP_SEND_TEXT;
    stale.dedup_key = cid;
    stale.request_id = cid;
    stale.payload = QString("{}");
    QList<OutboxEntryDTO> stale_list;
    stale_list.append(stale);
    emit LocalChatStore::GetInstance()->sig_outbox_loaded(true, stale_list);
    Pump(600);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 1,
          "out-of-restore snapshot ignored (no resurrection)");
    Check(!OutboxRow(uid, cid, &gone), "outbox stays deleted after stale snapshot");

    std::printf("\n== Group D: duplicate 1302 after finalize ==\n");
    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::SUCCESS, cid, kSid, QStringLiteral("2026-08-29T10:00:00"));
    Pump(300);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 1,
          "duplicate ACK ignored after entry erased");
}

// ---------------------------------------------------------------------------
// Group E — 在线增量登记立即首发；transient 只做内存退避，退避到点重发一次。
// Group F — 永久冲突：markSendFailed 落库成功后停止重传。
// ---------------------------------------------------------------------------
void GroupEandF_OnlineDispatchBackoffAndConflict(const SendSpy& spy)
{
    std::printf("\n== Group E: online incremental dispatch + transient backoff ==\n");
    const int uid = 41002;
    OpenAndLogin(uid);
    const int base = spy.Snapshot(ID_TEXT_CHAT_MSG_REQ);

    LocalMessageDTO dto = MakeOutgoingText();
    const QString cid = dto.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(dto);
    Pump(30);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 1,
          "committed send dispatches immediately (incremental registration)");

    //transient：2s 初始退避内不重发
    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::SERVER_BUSY, cid, 0, QString());
    Pump(800);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 1,
          "transient error backs off (no resend within initial window)");
    //退避到点（2s+扫描粒度）恰好重发一次，随后退避翻倍
    Pump(1700);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base + 2,
          "resend fires once after backoff deadline");

    //收尾：成功销账，避免后续扫描继续重发
    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::SUCCESS, cid, 990002, QStringLiteral("2026-08-29T10:01:00"));
    Pump(100);
    LocalMessageDTO stored;
    Check(ReadMessageRow(uid, cid, &stored) && stored.send_state == SEND_STATE_SENT,
          "transient-retried message finally confirmed");

    std::printf("\n== Group F: permanent conflict -> markSendFailed boundary ==\n");
    const int uid2 = 41003;
    OpenAndLogin(uid2);
    const int base2 = spy.Snapshot(ID_TEXT_CHAT_MSG_REQ);
    LocalMessageDTO dto2 = MakeOutgoingText();
    const QString cid2 = dto2.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(dto2);
    Pump(30);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base2 + 1, "conflict-case message sent once");

    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::MESSAGE_CONFLICT, cid2, 0, QString());
    Pump(600);
    Check(spy.Count(ID_TEXT_CHAT_MSG_REQ) == base2 + 1,
          "no resend after permanent conflict");
    LocalMessageDTO failed;
    Check(ReadMessageRow(uid2, cid2, &failed), "row readable after conflict");
    Check(failed.send_state == SEND_STATE_FAILED, "send_state=failed after conflict");
    OutboxEntryDTO gone;
    Check(!OutboxRow(uid2, cid2, &gone), "outbox deleted after markSendFailed commit");
}

// ---------------------------------------------------------------------------
// Group G — 资源 1504 成功：stage=uploading 落库成功后才停止重发 1503，
//           server_message_id 同时落库；不再重发 1503。
// Group H — 资源源文件丢失：登记后立即按 MARK_FAILED 收尾（落库后删登记）。
// ---------------------------------------------------------------------------
void GroupGandH_ResourceStageBoundary(const SendSpy& spy)
{
    std::printf("\n== Group G: 1504 SUCCESS -> stage advance commit boundary ==\n");
    QTemporaryDir dir;
    Check(dir.isValid(), "temporary dir created");
    const int uid = 41004;
    OpenAndLogin(uid);
    const int base = spy.Snapshot(ID_CREATE_RESOURCE_MSG_REQ);
    const int progress_base = spy.Snapshot(ID_RESOURCE_UPLOAD_PROGRESS_REQ);

    const QString image_path = dir.path() + "/pic.png";
    {
        QFile file(image_path);
        file.open(QIODevice::WriteOnly);
        file.write("fake-png-bytes");
        file.close();
    }

    LocalMessageDTO dto;
    dto.client_message_id = NextClientId("img");
    dto.thread_id = kThread;
    dto.sender_id = kSelf;
    dto.receiver_id = kPeer;
    dto.message_type = 1;
    dto.content = QStringLiteral("pic.png");
    dto.local_path = image_path;
    dto.content_size = QStringLiteral("15");
    dto.content_hash = QStringLiteral(
        "f32b67c7c2631af8f3c4b9d41c71ef0c3b8a4c6a1d2e3f4051627384950a1b2c");
    dto.mime_type = QStringLiteral("image/png");
    const QString cid = dto.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(dto);
    Pump(30);
    Check(spy.Count(ID_CREATE_RESOURCE_MSG_REQ) == base + 1, "1503 sent once");

    const qint64 kSid = 880001;
    emit TcpMgr::GetInstance()->sig_resource_msg_meta_rsp_forward(
        ErrorCodes::SUCCESS, cid, QStringLiteral("pic.png"), kSid, kThread, kSelf, kPeer);
    Pump(300);
    Check(spy.Count(ID_CREATE_RESOURCE_MSG_REQ) == base + 1,
          "1503 not resent after 1504 (stage advance path)");

    LocalMessageDTO staged;
    Check(ReadMessageRow(uid, cid, &staged), "resource row readable after 1504");
    Check(staged.server_message_id == kSid, "1504 server_message_id persisted");
    Check(staged.send_state == SEND_STATE_SENDING, "still sending during upload");
    OutboxEntryDTO row;
    Check(OutboxRow(uid, cid, &row), "outbox entry kept during uploading");
    Check(row.stage == RESOURCE_STAGE_UPLOADING, "outbox stage=uploading committed");
    Check(spy.Count(ID_RESOURCE_UPLOAD_PROGRESS_REQ) == progress_base,
          "1507 waits for ResourceServer authentication");

    emit FileTcpMgr::GetInstance()->sig_resource_login_success();
    Pump(30);
    Check(spy.Count(ID_RESOURCE_UPLOAD_PROGRESS_REQ) == progress_base + 1,
          "resource ready resumes uploading entry with one 1507");

    std::printf("\n== Group H: missing source file -> immediate MARK_FAILED ==\n");
    const int uid2 = 41005;
    OpenAndLogin(uid2);
    LocalMessageDTO dto2;
    dto2.client_message_id = NextClientId("img");
    dto2.thread_id = kThread;
    dto2.sender_id = kSelf;
    dto2.receiver_id = kPeer;
    dto2.message_type = 1;
    dto2.content = QStringLiteral("gone.png");
    dto2.local_path = dir.path() + "/gone.png";   //不存在
    dto2.content_size = QStringLiteral("15");
    dto2.content_hash = QStringLiteral(
        "f32b67c7c2631af8f3c4b9d41c71ef0c3b8a4c6a1d2e3f4051627384950a1b2c");
    dto2.mime_type = QStringLiteral("image/png");
    const QString cid2 = dto2.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(dto2);
    Pump(300);
    LocalMessageDTO failed;
    Check(ReadMessageRow(uid2, cid2, &failed), "missing-file row readable");
    Check(failed.send_state == SEND_STATE_FAILED,
          "missing source file marks failed (after store commit)");
    OutboxEntryDTO gone;
    Check(!OutboxRow(uid2, cid2, &gone), "missing-file outbox entry removed");
}

int main(int argc, char* argv[])
{
    //先启用测试目录隔离，再创建任何可能触碰 QStandardPaths 的对象
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("outbox_dispatcher_tests_%1")
        .arg(QCoreApplication::applicationPid()));
    const QString test_root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    //单例构造即完成信号接线（Dispatcher ctor 拉起 TcpMgr/FileTcpMgr/Store/UserMgr）
    OutboxDispatcher::GetInstance();
    TcpMgr::GetInstance();
    LocalChatStore::GetInstance();

    SendSpy spy;
    //无上下文 lambda → direct 连接；测试内所有发射都发生在主线程
    QObject::connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_send_data,
        [&spy](ReqId id, const QByteArray&) {
            std::fprintf(stderr, "[SPY] send_data id=%d\n", static_cast<int>(id));
            spy.counts[static_cast<int>(id)] += 1;
        });
    QObject::connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_send_data,
        [&spy](ReqId id, const QByteArray&) {
            std::fprintf(stderr, "[SPY] file_send_data id=%d\n", static_cast<int>(id));
            spy.counts[static_cast<int>(id)] += 1;
        });
    QObject::connect(LocalChatStore::GetInstance().get(), &LocalChatStore::sig_send_enqueued,
        [](bool ok, LocalMessageDTO dto, OutboxEntryDTO entry) {
            std::fprintf(stderr, "[DBG] sig_send_enqueued ok=%d opid=%lld type=%s payload-empty=%d\n",
                ok ? 1 : 0, static_cast<long long>(entry.operation_id),
                qPrintable(entry.operation_type), entry.payload.isEmpty() ? 1 : 0);
        });
    QObject::connect(LocalChatStore::GetInstance().get(), &LocalChatStore::sig_outbox_loaded,
        [](bool ok, QList<OutboxEntryDTO> es) {
            std::fprintf(stderr, "[DBG] sig_outbox_loaded ok=%d n=%d\n",
                ok ? 1 : 0, es.size());
        });

    Group0_LoginBeforeDbOpen(spy);
    GroupAtoD_OfflineRestoreAndAckBoundary(spy);
    GroupEandF_OnlineDispatchBackoffAndConflict(spy);
    GroupGandH_ResourceStageBoundary(spy);

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
        g_failures, g_failures == 1 ? "" : "s");
    const int result = g_failures == 0 ? 0 : 1;

    //静态单例析构顺序不确定；测试结束前先在其所属线程关闭连接，再清理本次唯一目录
    LocalChatStore::GetInstance()->closeDb();
    Pump(30);
    QDir(test_root).removeRecursively();
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(result);
}
