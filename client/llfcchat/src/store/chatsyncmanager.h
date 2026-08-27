#ifndef CHATSYNCMANAGER_H
#define CHATSYNCMANAGER_H

#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QTimer>

#include "localmessageDTO.h"
#include "singleton.h"
#include "userdata.h"

class ChatSyncManager : public QObject, public Singleton<ChatSyncManager>,
	public std::enable_shared_from_this<ChatSyncManager>
{
	Q_OBJECT
public:
	friend class Singleton<ChatSyncManager>;
	~ChatSyncManager();

public slots:
	void slot_login_snapshot(QJsonObject snapshot, bool initialLogin);
	void slot_user_message_notify(QJsonObject envelope);
	void slot_business_response(QJsonObject envelope);
	void slot_sync_message_rsp(QJsonObject rsp);
	void slot_load_chat_thread(bool loadMore, qint64 nextLastId,
		std::vector<std::shared_ptr<ChatThreadInfo> > chatThreads);

private slots:
	void slot_start();
	void slot_sync_state_loaded(bool ok, qint64 lastRecvSeq, bool bootstrapComplete);
	void slot_conversations_upserted(bool ok);
	void slot_snapshot_applied(bool ok);
	void slot_bootstrap_marked(bool ok, qint64 checkpoint);
	void slot_sync_page_applied(bool ok, qint64 newRecvSeq,
		QList<LocalMessageDTO> msgs, QList<qint64> insertedIds);
	void slot_periodic_sync();
	void slot_connection_closed();

private:
	ChatSyncManager();
	enum SyncState {
		SYNC_IDLE,
		SYNC_WAIT_STATE,
		SYNC_BOOTSTRAP_THREADS,
		SYNC_WAIT_UPSERT,
		SYNC_WAIT_SNAPSHOT,
		SYNC_WAIT_MARK,
		SYNC_WAIT_PAGE
	};
	void sendSyncRequest();
	void sendThreadListRequest(qint64 lastThreadId);
	void applyNextLiveIfPossible();
	void schedulePeriodicSync();
	static LocalMessageDTO envelopeToDto(const QJsonObject& envelope);

	SyncState _state;
	qint64 _last_recv_seq;
	qint64 _checkpoint;
	bool _has_more_pending;
	bool _pull_pending;
	bool _applying_live;
	bool _mark_after_snapshot;
	QJsonArray _snapshot_requests;
	QJsonArray _snapshot_contacts;
	QMap<qint64, QJsonObject> _pending_live;
	QList<LocalConversationDTO> _bootstrap_convs;
	QTimer* _periodic_timer;
};

#endif
