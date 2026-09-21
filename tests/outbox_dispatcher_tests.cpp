// OutboxDispatcher tests for the current messages/resources/outbox model.

#include "filetcpmgr.h"
#include "global.h"
#include "localchatdb.h"
#include "localchatstore.h"
#include "outboxdispatcher.h"
#include "tcpmgr.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>

namespace {

const qint64 kSelf = 1001;
const qint64 kPeer = 2002;
const qint64 kThread = 35;

int gFailures = 0;
qint64 gClientSequence = 0;
bool gLastDbOpenOk = false;
int gLastDbOpenUserId = 0;
bool gLastEnqueueOk = false;
QString gLastEnqueueClientMessageId;

void Check(bool condition, const char* name) {
    if (condition) {
        std::printf("[PASS] %s\n", name);
    } else {
        std::printf("[FAIL] %s\n", name);
        ++gFailures;
    }
}

void Pump(int milliseconds) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!elapsed.hasExpired(milliseconds)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

QString NextClientMessageId(const char* prefix) {
    return QStringLiteral("%1-%2")
        .arg(QString::fromLatin1(prefix)).arg(++gClientSequence);
}

QString UserDbPath(int userId) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/user/") + QString::number(userId)
        + QStringLiteral("/chat.db");
}

LocalMessageDTO MakeText(const QString& clientMessageId) {
    LocalMessageDTO message;
    message.client_message_id = clientMessageId;
    message.thread_id = kThread;
    message.sender_user_id = kSelf;
    message.message_type = static_cast<int>(ChatMsgType::TEXT);
    message.text_content = QStringLiteral("hello");
    message.created_at = QDateTime::currentMSecsSinceEpoch();
    return message;
}

OutboxEntryDTO MakeOutbox(const QString& clientMessageId,
    const QString& payload) {
    OutboxEntryDTO entry;
    entry.client_message_id = clientMessageId;
    entry.payload_json = payload;
    return entry;
}

bool ReadMessage(int userId, const QString& clientMessageId,
    LocalMessageDTO* message, LocalMessageResourceDTO* resource = nullptr) {
    LocalChatDb db;
    return db.open(UserDbPath(userId), userId)
        && db.getMessageByClientId(clientMessageId, message, resource);
}

bool FindOutbox(int userId, const QString& clientMessageId,
    OutboxEntryDTO* found) {
    LocalChatDb db;
    if (!db.open(UserDbPath(userId), userId)) return false;
    QList<OutboxEntryDTO> entries;
    if (!db.loadOutbox(&entries)) return false;
    for (const OutboxEntryDTO& entry : entries) {
        if (entry.client_message_id != clientMessageId) continue;
        if (found) *found = entry;
        return true;
    }
    return false;
}

struct SendSpy {
    QHash<int, int> counts;
    QHash<int, QByteArray> payloads;
    int count(int messageId) const { return counts.value(messageId, 0); }
};

void OpenUser(int userId, bool login) {
    gLastDbOpenOk = false;
    gLastDbOpenUserId = 0;
    LocalChatStore::GetInstance()->openUserDb(userId);
    Pump(30);
    Check(gLastDbOpenOk && gLastDbOpenUserId == userId,
        "test user database opens");
    if (login) {
        emit TcpMgr::GetInstance()->sig_chat_login_ready();
        Pump(30);
    }
}

void TestOfflineRestoreAndTextConfirm(const SendSpy& spy) {
    const int userId = 41001;
    OpenUser(userId, false);

    const int before = spy.count(ID_TEXT_CHAT_MSG_REQ);
    LocalMessageDTO message = MakeText(NextClientMessageId("text"));
    LocalMessageResourceDTO resource;
    OutboxEntryDTO outbox = MakeOutbox(message.client_message_id,
        QStringLiteral("{\"client_message_id\":\"%1\","
            "\"thread_id\":\"35\",\"target_user_id\":2002,"
            "\"text_content\":\"hello\"}")
            .arg(message.client_message_id));
    const QString clientMessageId = message.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(message, resource, outbox);
    Pump(30);
    Check(gLastEnqueueOk && gLastEnqueueClientMessageId == clientMessageId,
        "offline text enqueue transaction commits");

    Check(spy.count(ID_TEXT_CHAT_MSG_REQ) == before,
        "offline committed text is not sent");
    Check(FindOutbox(userId, clientMessageId, nullptr),
        "offline text is durable in outbox");

    emit TcpMgr::GetInstance()->sig_chat_login_ready();
    Pump(100);
    Check(spy.count(ID_TEXT_CHAT_MSG_REQ) == before + 1,
        "login restore sends durable text once");

    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::SUCCESS, clientMessageId, 9000000001LL,
        QStringLiteral("2026-08-30T10:00:00.000Z"));
    Pump(100);

    LocalMessageDTO stored;
    Check(ReadMessage(userId, clientMessageId, &stored)
        && stored.message_id == 9000000001LL
        && stored.send_status == LOCAL_SEND_SENT,
        "1302 commit stores canonical message id and sent status");
    Check(!FindOutbox(userId, clientMessageId, nullptr),
        "1302 commit removes outbox row");

    const int afterConfirm = spy.count(ID_TEXT_CHAT_MSG_REQ);
    OutboxEntryDTO stale = outbox;
    stale.operation_id = 99999;
    QList<OutboxEntryDTO> staleSnapshot;
    staleSnapshot << stale;
    emit LocalChatStore::GetInstance()->sig_outbox_loaded(true, staleSnapshot);
    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::SUCCESS, clientMessageId, 9000000001LL,
        QStringLiteral("2026-08-30T10:00:00.000Z"));
    Pump(300);
    Check(spy.count(ID_TEXT_CHAT_MSG_REQ) == afterConfirm,
        "stale snapshot and duplicate response cannot resurrect text");
}

void TestPermanentConflict(const SendSpy& spy) {
    const int userId = 41002;
    OpenUser(userId, true);

    const int before = spy.count(ID_TEXT_CHAT_MSG_REQ);
    LocalMessageDTO message = MakeText(NextClientMessageId("conflict"));
    LocalMessageResourceDTO resource;
    OutboxEntryDTO outbox = MakeOutbox(message.client_message_id,
        QStringLiteral("{\"client_message_id\":\"%1\","
            "\"thread_id\":\"35\",\"target_user_id\":2002,"
            "\"text_content\":\"hello\"}")
            .arg(message.client_message_id));
    const QString clientMessageId = message.client_message_id;
    LocalChatStore::GetInstance()->enqueueSend(message, resource, outbox);
    Pump(30);
    Check(gLastEnqueueOk && gLastEnqueueClientMessageId == clientMessageId,
        "online text enqueue transaction commits");
    Check(spy.count(ID_TEXT_CHAT_MSG_REQ) == before + 1,
        "online text dispatches after SQLite commit");

    emit TcpMgr::GetInstance()->sig_text_msg_rsp_forward(
        ErrorCodes::MESSAGE_CONFLICT, clientMessageId, 0, QString());
    Pump(100);
    LocalMessageDTO stored;
    Check(ReadMessage(userId, clientMessageId, &stored)
        && stored.send_status == LOCAL_SEND_FAILED,
        "message conflict persists failed status");
    Check(!FindOutbox(userId, clientMessageId, nullptr),
        "message conflict removes outbox after local commit");
}

void TestResourceStateMachine(const SendSpy& spy) {
    QTemporaryDir sourceDirectory;
    Check(sourceDirectory.isValid(), "resource source directory created");
    const QString sourcePath = sourceDirectory.filePath(QStringLiteral("photo.png"));
    {
        QFile file(sourcePath);
        Check(file.open(QIODevice::WriteOnly)
            && file.write("resource-data") == 13,
            "resource source file created");
    }

    const int userId = 41003;
    OpenUser(userId, true);
    const int createBefore = spy.count(ID_CREATE_RESOURCE_MSG_REQ);
    const int progressBefore = spy.count(ID_RESOURCE_UPLOAD_PROGRESS_REQ);

    const QString clientMessageId = NextClientMessageId("resource");
    LocalMessageDTO message;
    message.client_message_id = clientMessageId;
    message.thread_id = kThread;
    message.sender_user_id = kSelf;
    message.message_type = static_cast<int>(ChatMsgType::PIC);
    message.created_at = QDateTime::currentMSecsSinceEpoch();
    LocalMessageResourceDTO resource;
    resource.original_file_name = QStringLiteral("photo.png");
    resource.local_file_path = sourcePath;
    resource.file_size_bytes = 13;
    resource.sha256 = QString(64, QLatin1Char('a'));
    resource.mime_type = QStringLiteral("image/png");
    OutboxEntryDTO outbox = MakeOutbox(clientMessageId,
        QStringLiteral("{\"client_message_id\":\"%1\","
            "\"thread_id\":\"35\",\"target_user_id\":2002,"
            "\"message_type\":1,\"original_file_name\":\"photo.png\","
            "\"file_size_bytes\":\"13\",\"sha256\":\"%2\","
            "\"mime_type\":\"image/png\"}")
            .arg(clientMessageId, resource.sha256));
    LocalChatStore::GetInstance()->enqueueSend(message, resource, outbox);
    Pump(30);
    Check(gLastEnqueueOk && gLastEnqueueClientMessageId == clientMessageId,
        "resource enqueue transaction commits");
    Check(spy.count(ID_CREATE_RESOURCE_MSG_REQ) == createBefore + 1,
        "resource metadata dispatches once");

    emit TcpMgr::GetInstance()->sig_resource_msg_meta_rsp_forward(
        ErrorCodes::SUCCESS, clientMessageId, 9100000001LL);
    Pump(100);
    OutboxEntryDTO staged;
    LocalMessageDTO stored;
    Check(FindOutbox(userId, clientMessageId, &staged)
        && staged.stage == RESOURCE_STAGE_UPLOADING,
        "1504 commits message id and uploading stage");
    Check(ReadMessage(userId, clientMessageId, &stored)
        && stored.message_id == 9100000001LL
        && stored.send_status == LOCAL_SEND_SENDING,
        "resource remains sending while upload is incomplete");
    Check(spy.count(ID_RESOURCE_UPLOAD_PROGRESS_REQ) == progressBefore,
        "upload waits for ResourceServer authentication");

    emit FileTcpMgr::GetInstance()->sig_resource_login_success();
    Pump(50);
    Check(spy.count(ID_RESOURCE_UPLOAD_PROGRESS_REQ) == progressBefore + 1,
        "authenticated resource connection queries upload progress");

    const int chunksBefore = spy.count(ID_RESOURCE_CHUNK_UPLOAD_REQ);
    emit FileTcpMgr::GetInstance()->sig_upload_progress_rsp(
        9100000001LL, ErrorCodes::SUCCESS, resource.file_size_bytes,
        static_cast<int>(MessageStatus::Pending));
    Pump(50);
    Check(FindOutbox(userId, clientMessageId, nullptr)
        && ReadMessage(userId, clientMessageId, &stored)
        && stored.send_status == LOCAL_SEND_SENDING,
        "full-length pending upload stays in outbox until published");
    const QJsonObject retried = QJsonDocument::fromJson(
        spy.payloads.value(ID_RESOURCE_CHUNK_UPLOAD_REQ)).object();
    Check(spy.count(ID_RESOURCE_CHUNK_UPLOAD_REQ) == chunksBefore + 1
        && retried.value(QStringLiteral("offset")).toString() == QStringLiteral("0")
        && QByteArray::fromBase64(retried.value(QStringLiteral("data")).toString().toLatin1())
            == QByteArray("resource-data"),
        "full-length pending upload resends the final chunk from its start");

    const QString archivePath = sourceDirectory.filePath(QStringLiteral("9100000001"));
    QFile::copy(sourcePath, archivePath);
    emit FileTcpMgr::GetInstance()->sig_resource_upload_done(
        resource.original_file_name, archivePath);
    Pump(100);
    LocalMessageResourceDTO storedResource;
    Check(ReadMessage(userId, clientMessageId, &stored, &storedResource)
        && stored.send_status == LOCAL_SEND_SENT
        && storedResource.local_file_path == archivePath,
        "1506 commit stores sent status and archive path");
    Check(!FindOutbox(userId, clientMessageId, nullptr),
        "1506 commit removes resource outbox row");
}

void TestMissingResourceSource() {
    const int userId = 41004;
    OpenUser(userId, true);
    const QString clientMessageId = NextClientMessageId("missing");
    LocalMessageDTO message;
    message.client_message_id = clientMessageId;
    message.thread_id = kThread;
    message.sender_user_id = kSelf;
    message.message_type = static_cast<int>(ChatMsgType::FILE);
    message.created_at = QDateTime::currentMSecsSinceEpoch();
    LocalMessageResourceDTO resource;
    resource.original_file_name = QStringLiteral("missing.bin");
    resource.local_file_path = QStringLiteral("Z:/definitely/missing.bin");
    resource.file_size_bytes = 12;
    resource.sha256 = QString(64, QLatin1Char('b'));
    resource.mime_type = QStringLiteral("application/octet-stream");
    OutboxEntryDTO outbox = MakeOutbox(clientMessageId,
        QStringLiteral("{\"client_message_id\":\"%1\","
            "\"thread_id\":\"35\",\"target_user_id\":2002,"
            "\"message_type\":3,\"original_file_name\":\"missing.bin\","
            "\"file_size_bytes\":\"12\",\"sha256\":\"%2\","
            "\"mime_type\":\"application/octet-stream\"}")
            .arg(clientMessageId, resource.sha256));
    LocalChatStore::GetInstance()->enqueueSend(message, resource, outbox);
    Pump(100);
    Check(gLastEnqueueOk && gLastEnqueueClientMessageId == clientMessageId,
        "missing resource enqueue transaction commits");

    LocalMessageDTO stored;
    Check(ReadMessage(userId, clientMessageId, &stored)
        && stored.send_status == LOCAL_SEND_FAILED,
        "missing resource source is persisted as failed");
    Check(!FindOutbox(userId, clientMessageId, nullptr),
        "missing resource source removes outbox after failure commit");
}

} // namespace

int main(int argc, char* argv[]) {
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("outbox_dispatcher_tests_%1")
        .arg(QCoreApplication::applicationPid()));
    const QString testRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    OutboxDispatcher::GetInstance();
    TcpMgr::GetInstance();
    FileTcpMgr::GetInstance();
    LocalChatStore::GetInstance();

    QObject::connect(LocalChatStore::GetInstance().get(),
        &LocalChatStore::sig_db_opened,
        [](bool ok, int userId) {
            gLastDbOpenOk = ok;
            gLastDbOpenUserId = userId;
        });
    QObject::connect(LocalChatStore::GetInstance().get(),
        &LocalChatStore::sig_send_enqueued,
        [](bool ok, const LocalMessageDTO& message,
            const LocalMessageResourceDTO&, const OutboxEntryDTO&) {
            gLastEnqueueOk = ok;
            gLastEnqueueClientMessageId = message.client_message_id;
        });

    SendSpy spy;
    QObject::connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_send_data,
        [&spy](ReqId id, const QByteArray&) {
            spy.counts[static_cast<int>(id)] += 1;
        });
    QObject::connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_send_data,
        [&spy](ReqId id, const QByteArray& data) {
            spy.counts[static_cast<int>(id)] += 1;
            spy.payloads[static_cast<int>(id)] = data;
        });

    TestOfflineRestoreAndTextConfirm(spy);
    TestPermanentConflict(spy);
    TestResourceStateMachine(spy);
    TestMissingResourceSource();

    std::printf("\n%s: %d failure(s)\n",
        gFailures == 0 ? "ALL PASS" : "FAILED", gFailures);
    const int result = gFailures == 0 ? 0 : 1;

    LocalChatStore::GetInstance()->closeDb();
    Pump(30);
    const QDir root(testRoot);
    if (root.exists() && root.absolutePath().contains(
        QStringLiteral("outbox_dispatcher_tests_"))) {
        QDir(root).removeRecursively();
    }
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(result);
}
