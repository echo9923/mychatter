#ifndef LOCALCHATDB_H
#define LOCALCHATDB_H

#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "localmessageDTO.h"

const QString OUTBOX_OP_SEND_TEXT = QStringLiteral("SEND_TEXT");
const QString OUTBOX_OP_SEND_RESOURCE = QStringLiteral("SEND_RESOURCE");
const QString RESOURCE_STAGE_METADATA = QStringLiteral("metadata");
const QString RESOURCE_STAGE_UPLOADING = QStringLiteral("uploading");

enum LocalSendStatus {
	LOCAL_SEND_SENDING = 0,
	LOCAL_SEND_SENT = 1,
	LOCAL_SEND_FAILED = 2
};

class LocalChatDb {
public:
	explicit LocalChatDb(const QString& connectionName = QString());
	~LocalChatDb();

	bool open(const QString& dbPath, qint64 selfUserId);
	void close();
	bool isOpen() const;

	bool enqueueSend(LocalMessageDTO& message, LocalMessageResourceDTO& resource,
		OutboxEntryDTO& outbox);
	bool confirmTextSent(const QString& clientMessageId, qint64 messageId,
		qint64 createdAt, LocalMessageDTO* out);
	bool updateResourceStage(const QString& clientMessageId, qint64 messageId,
		const QString& stage, LocalMessageDTO* out);
	bool confirmResourceSent(const QString& clientMessageId, const QString& localFilePath,
		LocalMessageDTO* out);
	bool updateResourceLocalPath(qint64 messageId, const QString& localFilePath);
	bool markSendFailed(const QString& clientMessageId, LocalMessageDTO* out);
	bool getMessageByClientId(const QString& clientMessageId, LocalMessageDTO* message,
		LocalMessageResourceDTO* resource);

	bool insertIncoming(const QList<LocalMessageDTO>& messages,
		const QList<LocalMessageResourceDTO>& resources,
		QList<qint64>* insertedMessageIds = nullptr);
	bool applySyncPage(const QList<UserEventDTO>& events,
		const QList<LocalMessageDTO>& messages,
		const QList<LocalMessageResourceDTO>& resources,
		const QList<LocalFriendRequestDTO>& friendRequests,
		const QList<LocalContactDTO>& contacts,
		qint64 newEventSeq, QList<qint64>* insertedMessageIds = nullptr);
	bool insertHistoryPage(qint64 threadId, const QList<LocalMessageDTO>& messages,
		const QList<LocalMessageResourceDTO>& resources, bool historyComplete);
	bool upsertConversations(const QList<LocalConversationDTO>& conversations);
	bool applySnapshot(const QList<LocalFriendRequestDTO>& friendRequests,
		const QList<LocalContactDTO>& contacts, bool replaceCurrent);

	bool getSyncState(qint64* lastEventSeq, bool* bootstrapComplete);
	bool markBootstrapComplete(qint64 checkpoint);
	bool loadOutbox(QList<OutboxEntryDTO>* entries);
	bool loadConversations(QList<LocalConversationDTO>* conversations);
	bool loadRecentMessages(qint64 threadId, int limit,
		QList<LocalMessageDTO>* messages, QList<LocalMessageResourceDTO>* resources);
	bool loadOlderMessages(qint64 threadId, qint64 beforeMessageId, int limit,
		QList<LocalMessageDTO>* messages, QList<LocalMessageResourceDTO>* resources);

private:
	bool initSchema();
	bool insertMessageIgnore(const LocalMessageDTO& message,
		const LocalMessageResourceDTO& resource, bool* inserted, qint64* localMessageId);
	bool upsertConversationOnMessage(const LocalMessageDTO& message);
	bool upsertFriendRequest(const LocalFriendRequestDTO& request);
	bool upsertContact(const LocalContactDTO& contact);
	bool applyEvent(const UserEventDTO& event,
		const QList<LocalMessageDTO>& messages,
		const QList<LocalMessageResourceDTO>& resources,
		const QList<LocalFriendRequestDTO>& friendRequests,
		const QList<LocalContactDTO>& contacts,
		QList<qint64>* insertedMessageIds);
	LocalMessageDTO readMessageRow(class QSqlQuery& query) const;
	LocalMessageResourceDTO readResourceRow(class QSqlQuery& query, int firstColumn) const;
	bool fillMessageByClientId(const QString& clientMessageId, LocalMessageDTO* message,
		LocalMessageResourceDTO* resource = nullptr);
	bool loadMessages(const QString& sql, const QVariantList& binds,
		QList<LocalMessageDTO>* messages, QList<LocalMessageResourceDTO>* resources);

	QSqlDatabase _db;
	QString _connection_name;
	qint64 _self_user_id = 0;
};

#endif
