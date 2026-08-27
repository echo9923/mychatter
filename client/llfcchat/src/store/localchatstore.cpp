#include "localchatstore.h"
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

//———————————————— worker（只在 LocalChatThread 线程执行）————————————————

LocalChatWorker::LocalChatWorker(QObject* parent)
    : QObject(parent), _db("local_chat_store")
{
}

void LocalChatWorker::slot_open_user_db(int uid)
{
    //同 uid 已打开则直接复用
    if (_db.isOpen() && _uid == uid) {
        emit sig_db_opened(true, uid);
        return;
    }
    if (_db.isOpen()) {
        _db.close();
    }
    _uid = uid;
    QString storage_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString user_dir = storage_dir + "/user/" + QString::number(uid);
    QDir dir(user_dir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    bool ok = _db.open(user_dir + "/chat.db", uid);
    emit sig_db_opened(ok, uid);
}

void LocalChatWorker::slot_close_db()
{
    _db.close();
    _uid = 0;
    emit sig_db_closed();
}

void LocalChatWorker::slot_enqueue_send(LocalMessageDTO dto)
{
    bool ok = _db.enqueueSend(dto);
    emit sig_send_enqueued(ok, dto);
}

void LocalChatWorker::slot_confirm_text_sent(QString clientMessageId, qint64 serverMessageId,
    QString chatTime)
{
    LocalMessageDTO dto;
    bool ok = _db.confirmTextSent(clientMessageId, serverMessageId, chatTime, &dto);
    emit sig_send_confirmed(ok, dto);
}

void LocalChatWorker::slot_update_resource_stage(QString clientMessageId, qint64 serverMessageId,
    QString stage)
{
    LocalMessageDTO dto;
    bool ok = _db.updateResourceStage(clientMessageId, serverMessageId, stage, &dto);
    emit sig_resource_stage_updated(ok, dto);
}

void LocalChatWorker::slot_confirm_resource_sent(QString clientMessageId)
{
    LocalMessageDTO dto;
    bool ok = _db.confirmResourceSent(clientMessageId, &dto);
    emit sig_resource_confirmed(ok, dto);
}

void LocalChatWorker::slot_mark_send_failed(QString clientMessageId)
{
    LocalMessageDTO dto;
    bool ok = _db.markSendFailed(clientMessageId, &dto);
    emit sig_send_failed_marked(ok, dto);
}

void LocalChatWorker::slot_get_message_by_client_id(QString clientMessageId)
{
    LocalMessageDTO dto;
    bool ok = _db.getMessageByClientId(clientMessageId, &dto);
    emit sig_message_loaded(ok, dto);
}

void LocalChatWorker::slot_insert_incoming(QList<LocalMessageDTO> msgs)
{
    QList<qint64> inserted_ids;
    bool ok = _db.insertIncoming(msgs, &inserted_ids);
    emit sig_incoming_inserted(ok, msgs, inserted_ids);
}

void LocalChatWorker::slot_apply_sync_page(QList<LocalMessageDTO> msgs, qint64 newSyncSeq)
{
    QList<qint64> inserted_ids;
    bool ok = _db.applySyncPage(msgs, newSyncSeq, &inserted_ids);
    emit sig_sync_page_applied(ok, newSyncSeq, msgs, inserted_ids);
}

void LocalChatWorker::slot_insert_history_page(qint64 threadId, QList<LocalMessageDTO> msgs,
    bool historyComplete)
{
    bool ok = _db.insertHistoryPage(threadId, msgs, historyComplete);
    emit sig_history_page_inserted(ok, threadId);
}

void LocalChatWorker::slot_upsert_conversations(QList<LocalConversationDTO> convs)
{
    bool ok = _db.upsertConversations(convs);
    emit sig_conversations_upserted(ok);
}

void LocalChatWorker::slot_apply_snapshot(QJsonArray friendRequests,
	QJsonArray contacts, bool replaceCurrent)
{
	emit sig_snapshot_applied(_db.applySnapshot(friendRequests, contacts, replaceCurrent));
}

void LocalChatWorker::slot_get_sync_state()
{
    qint64 last_recv_seq = 0;
    bool bootstrap_complete = false;
    bool ok = _db.getSyncState(&last_recv_seq, &bootstrap_complete);
    emit sig_sync_state_loaded(ok, last_recv_seq, bootstrap_complete);
}

void LocalChatWorker::slot_mark_bootstrap_complete(qint64 checkpoint)
{
    bool ok = _db.markBootstrapComplete(checkpoint);
    emit sig_bootstrap_marked(ok, checkpoint);
}

void LocalChatWorker::slot_load_outbox()
{
    QList<OutboxEntryDTO> entries;
    bool ok = _db.loadOutbox(&entries);
    emit sig_outbox_loaded(ok, entries);
}

void LocalChatWorker::slot_delete_outbox_entry(QString dedupKey)
{
    _db.deleteOutboxEntry(dedupKey);
}

void LocalChatWorker::slot_update_outbox_retry(QString dedupKey, int retryCount,
    qint64 nextRetryAt)
{
    _db.updateOutboxRetry(dedupKey, retryCount, nextRetryAt);
}

void LocalChatWorker::slot_load_conversations()
{
    QList<LocalConversationDTO> convs;
    bool ok = _db.loadConversations(&convs);
    emit sig_conversations_loaded(ok, convs);
}

void LocalChatWorker::slot_load_recent_messages(qint64 threadId, int limit)
{
    QList<LocalMessageDTO> msgs;
    bool ok = _db.loadRecentMessages(threadId, limit, &msgs);
    bool history_complete = false;
    qint64 oldest_loaded = 0;
    if (ok) {
        //回读会话行（供 GUI 判断是否需要发 1403 拉更早历史）
        QList<LocalConversationDTO> convs;
        if (_db.loadConversations(&convs)) {
            for (const LocalConversationDTO& conv : convs) {
                if (conv.thread_id == threadId) {
                    history_complete = conv.history_complete;
                    oldest_loaded = conv.oldest_loaded_message_id;
                    break;
                }
            }
        }
    }
    emit sig_recent_messages_loaded(ok, threadId, msgs, history_complete, oldest_loaded);
}

void LocalChatWorker::slot_load_older_messages(qint64 threadId, qint64 beforeMessageId, int limit)
{
    QList<LocalMessageDTO> msgs;
    bool ok = _db.loadOlderMessages(threadId, beforeMessageId, limit, &msgs);
    bool history_complete = false;
    if (ok) {
        //回读会话行判断历史是否已拉全（供 GUI 决定是否继续发 1403）
        QList<LocalConversationDTO> convs;
        if (_db.loadConversations(&convs)) {
            for (const LocalConversationDTO& conv : convs) {
                if (conv.thread_id == threadId) {
                    history_complete = conv.history_complete;
                    break;
                }
            }
        }
    }
    emit sig_older_messages_loaded(ok, threadId, msgs, history_complete);
}

//———————————————— facade ————————————————

LocalChatStore::LocalChatStore()
{
    registerMetaType();
    //请求信号 → worker slot（worker 在 LocalChatThread 线程，自动 queued）
    connect(this, &LocalChatStore::sig_open_user_db, &_worker, &LocalChatWorker::slot_open_user_db);
    connect(this, &LocalChatStore::sig_close_db, &_worker, &LocalChatWorker::slot_close_db);
    connect(this, &LocalChatStore::sig_enqueue_send, &_worker, &LocalChatWorker::slot_enqueue_send);
    connect(this, &LocalChatStore::sig_confirm_text_sent, &_worker, &LocalChatWorker::slot_confirm_text_sent);
    connect(this, &LocalChatStore::sig_update_resource_stage, &_worker, &LocalChatWorker::slot_update_resource_stage);
    connect(this, &LocalChatStore::sig_confirm_resource_sent, &_worker, &LocalChatWorker::slot_confirm_resource_sent);
    connect(this, &LocalChatStore::sig_mark_send_failed, &_worker, &LocalChatWorker::slot_mark_send_failed);
    connect(this, &LocalChatStore::sig_get_message_by_client_id, &_worker, &LocalChatWorker::slot_get_message_by_client_id);
    connect(this, &LocalChatStore::sig_insert_incoming, &_worker, &LocalChatWorker::slot_insert_incoming);
    connect(this, &LocalChatStore::sig_apply_sync_page, &_worker, &LocalChatWorker::slot_apply_sync_page);
    connect(this, &LocalChatStore::sig_insert_history_page, &_worker, &LocalChatWorker::slot_insert_history_page);
    connect(this, &LocalChatStore::sig_upsert_conversations, &_worker, &LocalChatWorker::slot_upsert_conversations);
	connect(this, &LocalChatStore::sig_apply_snapshot, &_worker, &LocalChatWorker::slot_apply_snapshot);
    connect(this, &LocalChatStore::sig_get_sync_state, &_worker, &LocalChatWorker::slot_get_sync_state);
    connect(this, &LocalChatStore::sig_mark_bootstrap_complete, &_worker, &LocalChatWorker::slot_mark_bootstrap_complete);
    connect(this, &LocalChatStore::sig_load_outbox, &_worker, &LocalChatWorker::slot_load_outbox);
    connect(this, &LocalChatStore::sig_delete_outbox_entry, &_worker, &LocalChatWorker::slot_delete_outbox_entry);
    connect(this, &LocalChatStore::sig_update_outbox_retry, &_worker, &LocalChatWorker::slot_update_outbox_retry);
    connect(this, &LocalChatStore::sig_load_conversations, &_worker, &LocalChatWorker::slot_load_conversations);
    connect(this, &LocalChatStore::sig_load_recent_messages, &_worker, &LocalChatWorker::slot_load_recent_messages);
    connect(this, &LocalChatStore::sig_load_older_messages, &_worker, &LocalChatWorker::slot_load_older_messages);
    //worker 结果信号 → facade 结果信号透传（订阅方只依赖 facade）
    connect(&_worker, &LocalChatWorker::sig_db_opened, this, &LocalChatStore::sig_db_opened);
    connect(&_worker, &LocalChatWorker::sig_db_closed, this, &LocalChatStore::sig_db_closed);
    connect(&_worker, &LocalChatWorker::sig_send_enqueued, this, &LocalChatStore::sig_send_enqueued);
    connect(&_worker, &LocalChatWorker::sig_send_confirmed, this, &LocalChatStore::sig_send_confirmed);
    connect(&_worker, &LocalChatWorker::sig_resource_stage_updated, this, &LocalChatStore::sig_resource_stage_updated);
    connect(&_worker, &LocalChatWorker::sig_resource_confirmed, this, &LocalChatStore::sig_resource_confirmed);
    connect(&_worker, &LocalChatWorker::sig_send_failed_marked, this, &LocalChatStore::sig_send_failed_marked);
    connect(&_worker, &LocalChatWorker::sig_message_loaded, this, &LocalChatStore::sig_message_loaded);
    connect(&_worker, &LocalChatWorker::sig_incoming_inserted, this, &LocalChatStore::sig_incoming_inserted);
    connect(&_worker, &LocalChatWorker::sig_sync_page_applied, this, &LocalChatStore::sig_sync_page_applied);
    connect(&_worker, &LocalChatWorker::sig_history_page_inserted, this, &LocalChatStore::sig_history_page_inserted);
    connect(&_worker, &LocalChatWorker::sig_conversations_upserted, this, &LocalChatStore::sig_conversations_upserted);
	connect(&_worker, &LocalChatWorker::sig_snapshot_applied, this, &LocalChatStore::sig_snapshot_applied);
    connect(&_worker, &LocalChatWorker::sig_sync_state_loaded, this, &LocalChatStore::sig_sync_state_loaded);
    connect(&_worker, &LocalChatWorker::sig_bootstrap_marked, this, &LocalChatStore::sig_bootstrap_marked);
    connect(&_worker, &LocalChatWorker::sig_outbox_loaded, this, &LocalChatStore::sig_outbox_loaded);
    connect(&_worker, &LocalChatWorker::sig_conversations_loaded, this, &LocalChatStore::sig_conversations_loaded);
    connect(&_worker, &LocalChatWorker::sig_recent_messages_loaded, this, &LocalChatStore::sig_recent_messages_loaded);
    connect(&_worker, &LocalChatWorker::sig_older_messages_loaded, this, &LocalChatStore::sig_older_messages_loaded);
}

LocalChatStore::~LocalChatStore()
{
}

void LocalChatStore::registerMetaType()
{
    //跨线程 DTO 元类型注册
    qRegisterMetaType<LocalMessageDTO>("LocalMessageDTO");
    qRegisterMetaType<LocalConversationDTO>("LocalConversationDTO");
    qRegisterMetaType<OutboxEntryDTO>("OutboxEntryDTO");
    qRegisterMetaType<QList<LocalMessageDTO>>("QList<LocalMessageDTO>");
    qRegisterMetaType<QList<LocalConversationDTO>>("QList<LocalConversationDTO>");
    qRegisterMetaType<QList<OutboxEntryDTO>>("QList<OutboxEntryDTO>");
    qRegisterMetaType<QList<qint64>>("QList<qint64>");
}

void LocalChatStore::openUserDb(int uid) { emit sig_open_user_db(uid); }
void LocalChatStore::closeDb() { emit sig_close_db(); }
void LocalChatStore::enqueueSend(LocalMessageDTO dto) { emit sig_enqueue_send(dto); }
void LocalChatStore::confirmTextSent(const QString& clientMessageId, qint64 serverMessageId,
    const QString& chatTime)
{
    emit sig_confirm_text_sent(clientMessageId, serverMessageId, chatTime);
}
void LocalChatStore::updateResourceStage(const QString& clientMessageId, qint64 serverMessageId,
    const QString& stage)
{
    emit sig_update_resource_stage(clientMessageId, serverMessageId, stage);
}
void LocalChatStore::confirmResourceSent(const QString& clientMessageId)
{
    emit sig_confirm_resource_sent(clientMessageId);
}
void LocalChatStore::markSendFailed(const QString& clientMessageId)
{
    emit sig_mark_send_failed(clientMessageId);
}
void LocalChatStore::getMessageByClientId(const QString& clientMessageId)
{
    emit sig_get_message_by_client_id(clientMessageId);
}
void LocalChatStore::insertIncoming(const QList<LocalMessageDTO>& msgs)
{
    emit sig_insert_incoming(msgs);
}
void LocalChatStore::applySyncPage(const QList<LocalMessageDTO>& msgs, qint64 newSyncSeq)
{
    emit sig_apply_sync_page(msgs, newSyncSeq);
}
void LocalChatStore::insertHistoryPage(qint64 threadId, const QList<LocalMessageDTO>& msgs,
    bool historyComplete)
{
    emit sig_insert_history_page(threadId, msgs, historyComplete);
}
void LocalChatStore::upsertConversations(const QList<LocalConversationDTO>& convs)
{
    emit sig_upsert_conversations(convs);
}
void LocalChatStore::applySnapshot(const QJsonArray& friendRequests,
	const QJsonArray& contacts, bool replaceCurrent)
{
	emit sig_apply_snapshot(friendRequests, contacts, replaceCurrent);
}
void LocalChatStore::getSyncState() { emit sig_get_sync_state(); }
void LocalChatStore::markBootstrapComplete(qint64 checkpoint)
{
    emit sig_mark_bootstrap_complete(checkpoint);
}
void LocalChatStore::loadOutbox() { emit sig_load_outbox(); }
void LocalChatStore::deleteOutboxEntry(const QString& dedupKey)
{
    emit sig_delete_outbox_entry(dedupKey);
}
void LocalChatStore::updateOutboxRetry(const QString& dedupKey, int retryCount,
    qint64 nextRetryAt)
{
    emit sig_update_outbox_retry(dedupKey, retryCount, nextRetryAt);
}
void LocalChatStore::loadConversations() { emit sig_load_conversations(); }
void LocalChatStore::loadRecentMessages(qint64 threadId, int limit)
{
    emit sig_load_recent_messages(threadId, limit);
}
void LocalChatStore::loadOlderMessages(qint64 threadId, qint64 beforeMessageId, int limit)
{
    emit sig_load_older_messages(threadId, beforeMessageId, limit);
}

//———————————————— 线程启动（仿 TcpThread）————————————————

LocalChatThread::LocalChatThread()
{
    _local_chat_thread = new QThread();
    LocalChatStore::GetInstance()->worker()->moveToThread(_local_chat_thread);
    QObject::connect(_local_chat_thread, &QThread::finished, _local_chat_thread,
        &QObject::deleteLater);
    _local_chat_thread->start();
}

LocalChatThread::~LocalChatThread()
{
    _local_chat_thread->quit();
    _local_chat_thread->wait();
}
