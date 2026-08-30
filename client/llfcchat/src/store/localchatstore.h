#ifndef LOCALCHATSTORE_H
#define LOCALCHATSTORE_H

#include <QObject>
#include <QThread>

#include "localchatdb.h"
#include "localmessageDTO.h"
#include "singleton.h"

class LocalChatWorker : public QObject {
	Q_OBJECT
public:
	explicit LocalChatWorker(QObject* parent = nullptr);

public slots:
	void slot_open_user_db(int userId);
	void slot_close_db();
	void slot_enqueue_send(LocalMessageDTO message,
		LocalMessageResourceDTO resource, OutboxEntryDTO outbox);
	void slot_confirm_text_sent(QString clientMessageId, qint64 messageId, qint64 createdAt);
	void slot_update_resource_stage(QString clientMessageId, qint64 messageId, QString stage);
	void slot_confirm_resource_sent(QString clientMessageId, QString localFilePath);
	void slot_update_resource_local_path(qint64 messageId, QString localFilePath);
	void slot_mark_send_failed(QString clientMessageId);
	void slot_get_message_by_client_id(QString clientMessageId);
	void slot_insert_incoming(QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources);
	void slot_apply_sync_page(QList<UserEventDTO> events,
		QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources,
		QList<LocalFriendRequestDTO> friendRequests,
		QList<LocalContactDTO> contacts, qint64 newEventSeq);
	void slot_insert_history_page(qint64 threadId,
		QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources, bool historyComplete);
	void slot_upsert_conversations(QList<LocalConversationDTO> conversations);
	void slot_apply_snapshot(QList<LocalFriendRequestDTO> friendRequests,
		QList<LocalContactDTO> contacts, bool replaceCurrent);
	void slot_get_sync_state();
	void slot_mark_bootstrap_complete(qint64 checkpoint);
	void slot_load_outbox();
	void slot_load_conversations();
	void slot_load_recent_messages(qint64 threadId, int limit);
	void slot_load_older_messages(qint64 threadId, qint64 beforeMessageId, int limit);

signals:
	void sig_db_opened(bool ok, int userId);
	void sig_db_closed();
	void sig_send_enqueued(bool ok, LocalMessageDTO message,
		LocalMessageResourceDTO resource, OutboxEntryDTO outbox);
	void sig_send_confirmed(bool ok, LocalMessageDTO message);
	void sig_resource_stage_updated(bool ok, LocalMessageDTO message);
	void sig_resource_confirmed(bool ok, LocalMessageDTO message);
	void sig_resource_local_path_updated(bool ok, qint64 messageId, QString localFilePath);
	void sig_send_failed_marked(bool ok, LocalMessageDTO message);
	void sig_message_loaded(bool ok, LocalMessageDTO message,
		LocalMessageResourceDTO resource);
	void sig_incoming_inserted(bool ok, QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources, QList<qint64> insertedMessageIds);
	void sig_sync_page_applied(bool ok, qint64 newEventSeq,
		QList<UserEventDTO> events, QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources,
		QList<LocalFriendRequestDTO> friendRequests,
		QList<LocalContactDTO> contacts, QList<qint64> insertedMessageIds);
	void sig_history_page_inserted(bool ok, qint64 threadId);
	void sig_conversations_upserted(bool ok);
	void sig_snapshot_applied(bool ok);
	void sig_sync_state_loaded(bool ok, qint64 lastEventSeq, bool bootstrapComplete);
	void sig_bootstrap_marked(bool ok, qint64 checkpoint);
	void sig_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries);
	void sig_conversations_loaded(bool ok, QList<LocalConversationDTO> conversations);
	void sig_recent_messages_loaded(bool ok, qint64 threadId,
		QList<LocalMessageDTO> messages, QList<LocalMessageResourceDTO> resources,
		bool historyComplete, qint64 oldestLoadedMessageId);
	void sig_older_messages_loaded(bool ok, qint64 threadId,
		QList<LocalMessageDTO> messages, QList<LocalMessageResourceDTO> resources,
		bool historyComplete);

private:
	LocalChatDb _db;
	int _user_id = 0;
};

class LocalChatStore : public QObject, public Singleton<LocalChatStore>,
	public std::enable_shared_from_this<LocalChatStore> {
	Q_OBJECT
public:
	friend class Singleton<LocalChatStore>;
	~LocalChatStore();
	LocalChatWorker* worker() { return &_worker; }

	void openUserDb(int userId);
	void closeDb();
	void enqueueSend(const LocalMessageDTO& message,
		const LocalMessageResourceDTO& resource, const OutboxEntryDTO& outbox);
	void confirmTextSent(const QString& clientMessageId, qint64 messageId, qint64 createdAt);
	void updateResourceStage(const QString& clientMessageId, qint64 messageId,
		const QString& stage);
	void confirmResourceSent(const QString& clientMessageId, const QString& localFilePath);
	void updateResourceLocalPath(qint64 messageId, const QString& localFilePath);
	void markSendFailed(const QString& clientMessageId);
	void getMessageByClientId(const QString& clientMessageId);
	void insertIncoming(const QList<LocalMessageDTO>& messages,
		const QList<LocalMessageResourceDTO>& resources);
	void applySyncPage(const QList<UserEventDTO>& events,
		const QList<LocalMessageDTO>& messages,
		const QList<LocalMessageResourceDTO>& resources,
		const QList<LocalFriendRequestDTO>& friendRequests,
		const QList<LocalContactDTO>& contacts, qint64 newEventSeq);
	void insertHistoryPage(qint64 threadId, const QList<LocalMessageDTO>& messages,
		const QList<LocalMessageResourceDTO>& resources, bool historyComplete);
	void upsertConversations(const QList<LocalConversationDTO>& conversations);
	void applySnapshot(const QList<LocalFriendRequestDTO>& friendRequests,
		const QList<LocalContactDTO>& contacts, bool replaceCurrent);
	void getSyncState();
	void markBootstrapComplete(qint64 checkpoint);
	void loadOutbox();
	void loadConversations();
	void loadRecentMessages(qint64 threadId, int limit);
	void loadOlderMessages(qint64 threadId, qint64 beforeMessageId, int limit);

signals:
	void sig_open_user_db(int userId);
	void sig_close_db();
	void sig_enqueue_send(LocalMessageDTO message,
		LocalMessageResourceDTO resource, OutboxEntryDTO outbox);
	void sig_confirm_text_sent(QString clientMessageId, qint64 messageId, qint64 createdAt);
	void sig_update_resource_stage(QString clientMessageId, qint64 messageId, QString stage);
	void sig_confirm_resource_sent(QString clientMessageId, QString localFilePath);
	void sig_update_resource_local_path(qint64 messageId, QString localFilePath);
	void sig_mark_send_failed(QString clientMessageId);
	void sig_get_message_by_client_id(QString clientMessageId);
	void sig_insert_incoming(QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources);
	void sig_apply_sync_page(QList<UserEventDTO> events,
		QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources,
		QList<LocalFriendRequestDTO> friendRequests,
		QList<LocalContactDTO> contacts, qint64 newEventSeq);
	void sig_insert_history_page(qint64 threadId,
		QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources, bool historyComplete);
	void sig_upsert_conversations(QList<LocalConversationDTO> conversations);
	void sig_apply_snapshot(QList<LocalFriendRequestDTO> friendRequests,
		QList<LocalContactDTO> contacts, bool replaceCurrent);
	void sig_get_sync_state();
	void sig_mark_bootstrap_complete(qint64 checkpoint);
	void sig_load_outbox();
	void sig_load_conversations();
	void sig_load_recent_messages(qint64 threadId, int limit);
	void sig_load_older_messages(qint64 threadId, qint64 beforeMessageId, int limit);

	void sig_db_opened(bool ok, int userId);
	void sig_db_closed();
	void sig_send_enqueued(bool ok, LocalMessageDTO message,
		LocalMessageResourceDTO resource, OutboxEntryDTO outbox);
	void sig_send_confirmed(bool ok, LocalMessageDTO message);
	void sig_resource_stage_updated(bool ok, LocalMessageDTO message);
	void sig_resource_confirmed(bool ok, LocalMessageDTO message);
	void sig_resource_local_path_updated(bool ok, qint64 messageId, QString localFilePath);
	void sig_send_failed_marked(bool ok, LocalMessageDTO message);
	void sig_message_loaded(bool ok, LocalMessageDTO message,
		LocalMessageResourceDTO resource);
	void sig_incoming_inserted(bool ok, QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources, QList<qint64> insertedMessageIds);
	void sig_sync_page_applied(bool ok, qint64 newEventSeq,
		QList<UserEventDTO> events, QList<LocalMessageDTO> messages,
		QList<LocalMessageResourceDTO> resources,
		QList<LocalFriendRequestDTO> friendRequests,
		QList<LocalContactDTO> contacts, QList<qint64> insertedMessageIds);
	void sig_history_page_inserted(bool ok, qint64 threadId);
	void sig_conversations_upserted(bool ok);
	void sig_snapshot_applied(bool ok);
	void sig_sync_state_loaded(bool ok, qint64 lastEventSeq, bool bootstrapComplete);
	void sig_bootstrap_marked(bool ok, qint64 checkpoint);
	void sig_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries);
	void sig_conversations_loaded(bool ok, QList<LocalConversationDTO> conversations);
	void sig_recent_messages_loaded(bool ok, qint64 threadId,
		QList<LocalMessageDTO> messages, QList<LocalMessageResourceDTO> resources,
		bool historyComplete, qint64 oldestLoadedMessageId);
	void sig_older_messages_loaded(bool ok, qint64 threadId,
		QList<LocalMessageDTO> messages, QList<LocalMessageResourceDTO> resources,
		bool historyComplete);

private:
	LocalChatStore();
	void registerMetaTypes();
	LocalChatWorker _worker;
};

class LocalChatThread {
public:
	LocalChatThread();
	~LocalChatThread();
private:
	QThread* _thread;
};

#endif
