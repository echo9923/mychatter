#ifndef LOCALCHATSTORE_H
#define LOCALCHATSTORE_H

#include <QObject>
#include <QThread>
#include "singleton.h"
#include "localchatdb.h"
#include "localmessageDTO.h"

//本地库 worker：独占命名连接 QSQLITE（local_chat_store），只在 LocalChatThread 线程执行。
//GUI/TCP/File 线程一律不得触碰 QSqlDatabase，跨线程只走 queued 信号 + DTO。
class LocalChatWorker : public QObject
{
    Q_OBJECT
public:
    explicit LocalChatWorker(QObject* parent = nullptr);
public slots:
    void slot_open_user_db(int uid);
    void slot_close_db();
    void slot_enqueue_send(LocalMessageDTO dto);
    void slot_confirm_text_sent(QString clientMessageId, qint64 serverMessageId, QString chatTime);
    void slot_update_resource_stage(QString clientMessageId, qint64 serverMessageId, QString stage);
    void slot_confirm_resource_sent(QString clientMessageId);
    void slot_mark_send_failed(QString clientMessageId);
    void slot_get_message_by_client_id(QString clientMessageId);
    void slot_insert_incoming(QList<LocalMessageDTO> msgs);
    void slot_apply_sync_page(QList<LocalMessageDTO> msgs, qint64 newSyncSeq);
    void slot_insert_history_page(qint64 threadId, QList<LocalMessageDTO> msgs, bool historyComplete);
    void slot_upsert_conversations(QList<LocalConversationDTO> convs);
    void slot_get_sync_state();
    void slot_mark_bootstrap_complete(qint64 checkpoint);
    void slot_load_outbox();
    void slot_delete_outbox_entry(QString dedupKey);
    void slot_update_outbox_retry(QString dedupKey, int retryCount, qint64 nextRetryAt);
    void slot_load_conversations();
    void slot_load_recent_messages(qint64 threadId, int limit);
    void slot_load_older_messages(qint64 threadId, qint64 beforeMessageId, int limit);
signals:
    void sig_db_opened(bool ok, int uid);
    void sig_db_closed();
    void sig_send_enqueued(bool ok, LocalMessageDTO dto);
    void sig_send_confirmed(bool ok, LocalMessageDTO dto);
    void sig_resource_stage_updated(bool ok, LocalMessageDTO dto);
    void sig_resource_confirmed(bool ok, LocalMessageDTO dto);
    void sig_send_failed_marked(bool ok, LocalMessageDTO dto);
    void sig_message_loaded(bool ok, LocalMessageDTO dto);
    void sig_incoming_inserted(bool ok, QList<LocalMessageDTO> msgs, QList<qint64> insertedIds);
    void sig_sync_page_applied(bool ok, qint64 newSyncSeq, QList<LocalMessageDTO> msgs,
        QList<qint64> insertedIds);
    void sig_history_page_inserted(bool ok, qint64 threadId);
    void sig_conversations_upserted(bool ok);
    void sig_sync_state_loaded(bool ok, qint64 lastSyncSeq, bool bootstrapComplete);
    void sig_bootstrap_marked(bool ok, qint64 checkpoint);
    void sig_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries);
    void sig_conversations_loaded(bool ok, QList<LocalConversationDTO> convs);
    void sig_recent_messages_loaded(bool ok, qint64 threadId, QList<LocalMessageDTO> msgs,
        bool historyComplete, qint64 oldestLoadedMessageId);
    void sig_older_messages_loaded(bool ok, qint64 threadId, QList<LocalMessageDTO> msgs,
        bool historyComplete);
private:
    LocalChatDb _db;
    int _uid = 0;
};

//LocalChatStore：单例 QObject facade，公有 API 全部异步（请求信号进、结果信号出，
//结果信号带 threadId/clientMessageId 等关联字段）。
class LocalChatStore : public QObject, public Singleton<LocalChatStore>,
        public std::enable_shared_from_this<LocalChatStore>
{
    Q_OBJECT
public:
    friend class Singleton<LocalChatStore>;
    ~LocalChatStore();
    //worker 供 LocalChatThread moveToThread 使用
    LocalChatWorker* worker() { return &_worker; }

    void openUserDb(int uid);
    void closeDb();
    void enqueueSend(LocalMessageDTO dto);
    void confirmTextSent(const QString& clientMessageId, qint64 serverMessageId,
        const QString& chatTime);
    void updateResourceStage(const QString& clientMessageId, qint64 serverMessageId,
        const QString& stage);
    void confirmResourceSent(const QString& clientMessageId);
    void markSendFailed(const QString& clientMessageId);
    void getMessageByClientId(const QString& clientMessageId);
    void insertIncoming(const QList<LocalMessageDTO>& msgs);
    void applySyncPage(const QList<LocalMessageDTO>& msgs, qint64 newSyncSeq);
    void insertHistoryPage(qint64 threadId, const QList<LocalMessageDTO>& msgs,
        bool historyComplete);
    void upsertConversations(const QList<LocalConversationDTO>& convs);
    void getSyncState();
    void markBootstrapComplete(qint64 checkpoint);
    void loadOutbox();
    void deleteOutboxEntry(const QString& dedupKey);
    void updateOutboxRetry(const QString& dedupKey, int retryCount, qint64 nextRetryAt);
    void loadConversations();
    void loadRecentMessages(qint64 threadId, int limit);
    void loadOlderMessages(qint64 threadId, qint64 beforeMessageId, int limit);
signals:
    //—— 请求信号（facade → worker，queued 入 worker 线程）——
    void sig_open_user_db(int uid);
    void sig_close_db();
    void sig_enqueue_send(LocalMessageDTO dto);
    void sig_confirm_text_sent(QString clientMessageId, qint64 serverMessageId, QString chatTime);
    void sig_update_resource_stage(QString clientMessageId, qint64 serverMessageId, QString stage);
    void sig_confirm_resource_sent(QString clientMessageId);
    void sig_mark_send_failed(QString clientMessageId);
    void sig_get_message_by_client_id(QString clientMessageId);
    void sig_insert_incoming(QList<LocalMessageDTO> msgs);
    void sig_apply_sync_page(QList<LocalMessageDTO> msgs, qint64 newSyncSeq);
    void sig_insert_history_page(qint64 threadId, QList<LocalMessageDTO> msgs, bool historyComplete);
    void sig_upsert_conversations(QList<LocalConversationDTO> convs);
    void sig_get_sync_state();
    void sig_mark_bootstrap_complete(qint64 checkpoint);
    void sig_load_outbox();
    void sig_delete_outbox_entry(QString dedupKey);
    void sig_update_outbox_retry(QString dedupKey, int retryCount, qint64 nextRetryAt);
    void sig_load_conversations();
    void sig_load_recent_messages(qint64 threadId, int limit);
    void sig_load_older_messages(qint64 threadId, qint64 beforeMessageId, int limit);
    //—— 结果信号（worker → 各订阅方，在订阅方所在线程执行）——
    void sig_db_opened(bool ok, int uid);
    void sig_db_closed();
    void sig_send_enqueued(bool ok, LocalMessageDTO dto);
    void sig_send_confirmed(bool ok, LocalMessageDTO dto);
    void sig_resource_stage_updated(bool ok, LocalMessageDTO dto);
    void sig_resource_confirmed(bool ok, LocalMessageDTO dto);
    void sig_send_failed_marked(bool ok, LocalMessageDTO dto);
    void sig_message_loaded(bool ok, LocalMessageDTO dto);
    void sig_incoming_inserted(bool ok, QList<LocalMessageDTO> msgs, QList<qint64> insertedIds);
    void sig_sync_page_applied(bool ok, qint64 newSyncSeq, QList<LocalMessageDTO> msgs,
        QList<qint64> insertedIds);
    void sig_history_page_inserted(bool ok, qint64 threadId);
    void sig_conversations_upserted(bool ok);
    void sig_sync_state_loaded(bool ok, qint64 lastSyncSeq, bool bootstrapComplete);
    void sig_bootstrap_marked(bool ok, qint64 checkpoint);
    void sig_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries);
    void sig_conversations_loaded(bool ok, QList<LocalConversationDTO> convs);
    void sig_recent_messages_loaded(bool ok, qint64 threadId, QList<LocalMessageDTO> msgs,
        bool historyComplete, qint64 oldestLoadedMessageId);
    void sig_older_messages_loaded(bool ok, qint64 threadId, QList<LocalMessageDTO> msgs,
        bool historyComplete);
private:
    LocalChatStore();
    void registerMetaType();
    LocalChatWorker _worker;
};

//LocalChatThread：本地库 worker 线程的生命周期管理类（仿 TcpThread）。
//构造时 new 一个 QThread，把 LocalChatStore 单例里的 LocalChatWorker moveToThread 迁入该线程并
//start()，此后所有 SQLite 读写都经 queued 信号在 worker 线程串行执行，不阻塞 GUI 线程。
//析构时 quit()+wait() 同步退出；在 main 中作为栈对象构造，随进程存活。
class LocalChatThread {
public:
    LocalChatThread();
    ~LocalChatThread();
private:
    QThread* _local_chat_thread;
};

#endif // LOCALCHATSTORE_H
