#include "localchatstore.h"

#include <QDebug>
#include <QDir>
#include <QStandardPaths>

LocalChatWorker::LocalChatWorker(QObject* parent)
	: QObject(parent), _db(QStringLiteral("local_chat_store")) {
}

void LocalChatWorker::slot_open_user_db(int userId) {
	if (_db.isOpen() && _user_id == userId) {
		emit sig_db_opened(true, userId);
		return;
	}
	if (_db.isOpen()) _db.close();
	_user_id = userId;
	const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	const QString userDirectory = root + QStringLiteral("/user/") + QString::number(userId);
	QDir directory(userDirectory);
	if (!directory.exists()) directory.mkpath(QStringLiteral("."));
	emit sig_db_opened(_db.open(userDirectory + QStringLiteral("/chat.db"), userId), userId);
}

void LocalChatWorker::slot_close_db() {
	_db.close();
	_user_id = 0;
	emit sig_db_closed();
}

void LocalChatWorker::slot_enqueue_send(LocalMessageDTO message,
	LocalMessageResourceDTO resource, OutboxEntryDTO outbox) {
	const bool ok = _db.enqueueSend(message, resource, outbox);
	emit sig_send_enqueued(ok, message, resource, outbox);
}

void LocalChatWorker::slot_confirm_text_sent(QString clientMessageId,
	qint64 messageId, qint64 createdAt) {
	LocalMessageDTO message;
	const bool ok = _db.confirmTextSent(
		clientMessageId, messageId, createdAt, &message);
	emit sig_send_confirmed(ok, message);
}

void LocalChatWorker::slot_update_resource_stage(QString clientMessageId,
	qint64 messageId, QString stage) {
	LocalMessageDTO message;
	const bool ok = _db.updateResourceStage(
		clientMessageId, messageId, stage, &message);
	emit sig_resource_stage_updated(ok, message);
}

void LocalChatWorker::slot_confirm_resource_sent(QString clientMessageId,
	QString localFilePath) {
	LocalMessageDTO message;
	const bool ok = _db.confirmResourceSent(
		clientMessageId, localFilePath, &message);
	emit sig_resource_confirmed(ok, message);
}

void LocalChatWorker::slot_update_resource_local_path(qint64 messageId,
	QString localFilePath) {
	const bool ok = _db.updateResourceLocalPath(messageId, localFilePath);
	emit sig_resource_local_path_updated(ok, messageId, localFilePath);
}

void LocalChatWorker::slot_mark_send_failed(QString clientMessageId) {
	LocalMessageDTO message;
	const bool ok = _db.markSendFailed(clientMessageId, &message);
	emit sig_send_failed_marked(ok, message);
}

void LocalChatWorker::slot_get_message_by_client_id(QString clientMessageId) {
	LocalMessageDTO message;
	LocalMessageResourceDTO resource;
	const bool ok = _db.getMessageByClientId(clientMessageId, &message, &resource);
	emit sig_message_loaded(ok, message, resource);
}

void LocalChatWorker::slot_insert_incoming(QList<LocalMessageDTO> messages,
	QList<LocalMessageResourceDTO> resources) {
	QList<qint64> inserted;
	const bool ok = _db.insertIncoming(messages, resources, &inserted);
	emit sig_incoming_inserted(ok, messages, resources, inserted);
}

void LocalChatWorker::slot_apply_sync_page(QList<UserEventDTO> events,
	QList<LocalMessageDTO> messages, QList<LocalMessageResourceDTO> resources,
	QList<LocalFriendRequestDTO> friendRequests, QList<LocalContactDTO> contacts,
	qint64 newEventSeq) {
	QList<qint64> inserted;
	const bool ok = _db.applySyncPage(events, messages, resources, friendRequests,
		contacts, newEventSeq, &inserted);
	emit sig_sync_page_applied(ok, newEventSeq, events, messages, resources,
		friendRequests, contacts, inserted);
}

void LocalChatWorker::slot_insert_history_page(qint64 threadId,
	QList<LocalMessageDTO> messages, QList<LocalMessageResourceDTO> resources,
	bool historyComplete) {
	const bool ok = _db.insertHistoryPage(
		threadId, messages, resources, historyComplete);
	emit sig_history_page_inserted(ok, threadId);
}

void LocalChatWorker::slot_upsert_conversations(
	QList<LocalConversationDTO> conversations) {
	const bool ok = _db.upsertConversations(conversations);
	emit sig_conversations_upserted(ok);
}

void LocalChatWorker::slot_apply_snapshot(
	QList<LocalFriendRequestDTO> friendRequests, QList<LocalContactDTO> contacts,
	bool replaceCurrent) {
	const bool ok = _db.applySnapshot(friendRequests, contacts, replaceCurrent);
	emit sig_snapshot_applied(ok);
}

void LocalChatWorker::slot_get_sync_state() {
	qint64 lastEventSeq = 0;
	bool bootstrapComplete = false;
	const bool ok = _db.getSyncState(&lastEventSeq, &bootstrapComplete);
	emit sig_sync_state_loaded(ok, lastEventSeq, bootstrapComplete);
}

void LocalChatWorker::slot_mark_bootstrap_complete(qint64 checkpoint) {
	const bool ok = _db.markBootstrapComplete(checkpoint);
	emit sig_bootstrap_marked(ok, checkpoint);
}

void LocalChatWorker::slot_load_outbox() {
	QList<OutboxEntryDTO> entries;
	emit sig_outbox_loaded(_db.loadOutbox(&entries), entries);
}

void LocalChatWorker::slot_load_conversations() {
	QList<LocalConversationDTO> conversations;
	emit sig_conversations_loaded(_db.loadConversations(&conversations), conversations);
}

void LocalChatWorker::slot_load_recent_messages(qint64 threadId, int limit) {
	QList<LocalMessageDTO> messages;
	QList<LocalMessageResourceDTO> resources;
	const bool ok = _db.loadRecentMessages(threadId, limit, &messages, &resources);
	bool historyComplete = false;
	qint64 oldestLoadedMessageId = 0;
	if (ok) {
		QList<LocalConversationDTO> conversations;
		if (_db.loadConversations(&conversations)) {
			for (const LocalConversationDTO& conversation : conversations) {
				if (conversation.thread_id != threadId) continue;
				historyComplete = conversation.history_complete;
				oldestLoadedMessageId = conversation.oldest_loaded_message_id;
				break;
			}
		}
	}
	emit sig_recent_messages_loaded(ok, threadId, messages, resources,
		historyComplete, oldestLoadedMessageId);
}

void LocalChatWorker::slot_load_older_messages(qint64 threadId,
	qint64 beforeMessageId, int limit) {
	QList<LocalMessageDTO> messages;
	QList<LocalMessageResourceDTO> resources;
	const bool ok = _db.loadOlderMessages(threadId, beforeMessageId, limit,
		&messages, &resources);
	bool historyComplete = false;
	if (ok) {
		QList<LocalConversationDTO> conversations;
		if (_db.loadConversations(&conversations)) {
			for (const LocalConversationDTO& conversation : conversations) {
				if (conversation.thread_id == threadId) {
					historyComplete = conversation.history_complete;
					break;
				}
			}
		}
	}
	emit sig_older_messages_loaded(ok, threadId, messages, resources, historyComplete);
}

LocalChatStore::LocalChatStore() {
	registerMetaTypes();
	connect(this, &LocalChatStore::sig_open_user_db, &_worker, &LocalChatWorker::slot_open_user_db);
	connect(this, &LocalChatStore::sig_close_db, &_worker, &LocalChatWorker::slot_close_db);
	connect(this, &LocalChatStore::sig_enqueue_send, &_worker, &LocalChatWorker::slot_enqueue_send);
	connect(this, &LocalChatStore::sig_confirm_text_sent, &_worker, &LocalChatWorker::slot_confirm_text_sent);
	connect(this, &LocalChatStore::sig_update_resource_stage, &_worker, &LocalChatWorker::slot_update_resource_stage);
	connect(this, &LocalChatStore::sig_confirm_resource_sent, &_worker, &LocalChatWorker::slot_confirm_resource_sent);
	connect(this, &LocalChatStore::sig_update_resource_local_path, &_worker, &LocalChatWorker::slot_update_resource_local_path);
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
	connect(this, &LocalChatStore::sig_load_conversations, &_worker, &LocalChatWorker::slot_load_conversations);
	connect(this, &LocalChatStore::sig_load_recent_messages, &_worker, &LocalChatWorker::slot_load_recent_messages);
	connect(this, &LocalChatStore::sig_load_older_messages, &_worker, &LocalChatWorker::slot_load_older_messages);

	connect(&_worker, &LocalChatWorker::sig_db_opened, this, &LocalChatStore::sig_db_opened);
	connect(&_worker, &LocalChatWorker::sig_db_closed, this, &LocalChatStore::sig_db_closed);
	connect(&_worker, &LocalChatWorker::sig_send_enqueued, this, &LocalChatStore::sig_send_enqueued);
	connect(&_worker, &LocalChatWorker::sig_send_confirmed, this, &LocalChatStore::sig_send_confirmed);
	connect(&_worker, &LocalChatWorker::sig_resource_stage_updated, this, &LocalChatStore::sig_resource_stage_updated);
	connect(&_worker, &LocalChatWorker::sig_resource_confirmed, this, &LocalChatStore::sig_resource_confirmed);
	connect(&_worker, &LocalChatWorker::sig_resource_local_path_updated, this, &LocalChatStore::sig_resource_local_path_updated);
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

LocalChatStore::~LocalChatStore() {
}

void LocalChatStore::registerMetaTypes() {
	qRegisterMetaType<UserEventDTO>("UserEventDTO");
	qRegisterMetaType<LocalMessageDTO>("LocalMessageDTO");
	qRegisterMetaType<LocalMessageResourceDTO>("LocalMessageResourceDTO");
	qRegisterMetaType<LocalConversationDTO>("LocalConversationDTO");
	qRegisterMetaType<LocalFriendRequestDTO>("LocalFriendRequestDTO");
	qRegisterMetaType<LocalContactDTO>("LocalContactDTO");
	qRegisterMetaType<OutboxEntryDTO>("OutboxEntryDTO");
	qRegisterMetaType<QList<UserEventDTO>>("QList<UserEventDTO>");
	qRegisterMetaType<QList<LocalMessageDTO>>("QList<LocalMessageDTO>");
	qRegisterMetaType<QList<LocalMessageResourceDTO>>("QList<LocalMessageResourceDTO>");
	qRegisterMetaType<QList<LocalConversationDTO>>("QList<LocalConversationDTO>");
	qRegisterMetaType<QList<LocalFriendRequestDTO>>("QList<LocalFriendRequestDTO>");
	qRegisterMetaType<QList<LocalContactDTO>>("QList<LocalContactDTO>");
	qRegisterMetaType<QList<OutboxEntryDTO>>("QList<OutboxEntryDTO>");
	qRegisterMetaType<QList<qint64>>("QList<qint64>");
}

void LocalChatStore::openUserDb(int userId) { emit sig_open_user_db(userId); }
void LocalChatStore::closeDb() { emit sig_close_db(); }
void LocalChatStore::enqueueSend(const LocalMessageDTO& message,
	const LocalMessageResourceDTO& resource, const OutboxEntryDTO& outbox) {
	emit sig_enqueue_send(message, resource, outbox);
}
void LocalChatStore::confirmTextSent(const QString& clientMessageId,
	qint64 messageId, qint64 createdAt) {
	emit sig_confirm_text_sent(clientMessageId, messageId, createdAt);
}
void LocalChatStore::updateResourceStage(const QString& clientMessageId,
	qint64 messageId, const QString& stage) {
	emit sig_update_resource_stage(clientMessageId, messageId, stage);
}
void LocalChatStore::confirmResourceSent(const QString& clientMessageId,
	const QString& localFilePath) {
	emit sig_confirm_resource_sent(clientMessageId, localFilePath);
}
void LocalChatStore::updateResourceLocalPath(qint64 messageId,
	const QString& localFilePath) {
	emit sig_update_resource_local_path(messageId, localFilePath);
}
void LocalChatStore::markSendFailed(const QString& clientMessageId) {
	emit sig_mark_send_failed(clientMessageId);
}
void LocalChatStore::getMessageByClientId(const QString& clientMessageId) {
	emit sig_get_message_by_client_id(clientMessageId);
}
void LocalChatStore::insertIncoming(const QList<LocalMessageDTO>& messages,
	const QList<LocalMessageResourceDTO>& resources) {
	emit sig_insert_incoming(messages, resources);
}
void LocalChatStore::applySyncPage(const QList<UserEventDTO>& events,
	const QList<LocalMessageDTO>& messages,
	const QList<LocalMessageResourceDTO>& resources,
	const QList<LocalFriendRequestDTO>& friendRequests,
	const QList<LocalContactDTO>& contacts, qint64 newEventSeq) {
	emit sig_apply_sync_page(events, messages, resources, friendRequests, contacts, newEventSeq);
}
void LocalChatStore::insertHistoryPage(qint64 threadId,
	const QList<LocalMessageDTO>& messages,
	const QList<LocalMessageResourceDTO>& resources, bool historyComplete) {
	emit sig_insert_history_page(threadId, messages, resources, historyComplete);
}
void LocalChatStore::upsertConversations(
	const QList<LocalConversationDTO>& conversations) {
	emit sig_upsert_conversations(conversations);
}
void LocalChatStore::applySnapshot(
	const QList<LocalFriendRequestDTO>& friendRequests,
	const QList<LocalContactDTO>& contacts, bool replaceCurrent) {
	emit sig_apply_snapshot(friendRequests, contacts, replaceCurrent);
}
void LocalChatStore::getSyncState() { emit sig_get_sync_state(); }
void LocalChatStore::markBootstrapComplete(qint64 checkpoint) {
	emit sig_mark_bootstrap_complete(checkpoint);
}
void LocalChatStore::loadOutbox() { emit sig_load_outbox(); }
void LocalChatStore::loadConversations() { emit sig_load_conversations(); }
void LocalChatStore::loadRecentMessages(qint64 threadId, int limit) {
	emit sig_load_recent_messages(threadId, limit);
}
void LocalChatStore::loadOlderMessages(qint64 threadId,
	qint64 beforeMessageId, int limit) {
	emit sig_load_older_messages(threadId, beforeMessageId, limit);
}

LocalChatThread::LocalChatThread() : _thread(new QThread()) {
	LocalChatStore::GetInstance()->worker()->moveToThread(_thread);
	QObject::connect(_thread, &QThread::finished, _thread, &QObject::deleteLater);
	_thread->start();
}

LocalChatThread::~LocalChatThread() {
	_thread->quit();
	_thread->wait();
}
