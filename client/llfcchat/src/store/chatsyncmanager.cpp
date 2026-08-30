#include "chatsyncmanager.h"

#include "global.h"
#include "localchatstore.h"
#include "tcpmgr.h"
#include "usermgr.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>

namespace {

qint64 JsonId(const QJsonValue& value) {
	bool ok = false;
	const qint64 result = value.isString() ? value.toString().toLongLong(&ok) : 0;
	return ok ? result : 0;
}

qint64 ServerTimeToEpoch(const QJsonValue& value) {
	if (!value.isString()) return 0;
	const QString text = value.toString();
	QDateTime dateTime = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
	if (!dateTime.isValid()) dateTime = QDateTime::fromString(text, Qt::ISODateWithMs);
	if (!dateTime.isValid()) dateTime = QDateTime::fromString(text, Qt::ISODate);
	if (!dateTime.isValid()) return 0;
	dateTime.setTimeSpec(Qt::UTC);
	return dateTime.toMSecsSinceEpoch();
}

} // namespace

ChatSyncManager::ChatSyncManager()
	: _state(SYNC_IDLE), _last_event_seq(0), _checkpoint(0),
	  _has_more_pending(false), _pull_pending(false), _applying_live(false),
	  _mark_after_snapshot(false), _periodic_timer(new QTimer(this)) {
	auto tcp = TcpMgr::GetInstance();
	connect(tcp.get(), &TcpMgr::sig_chat_login_ready, this, &ChatSyncManager::slot_start);
	connect(tcp.get(), &TcpMgr::sig_login_snapshot, this, &ChatSyncManager::slot_login_snapshot);
	connect(tcp.get(), &TcpMgr::sig_user_message_notify,
		this, &ChatSyncManager::slot_user_message_notify);
	connect(tcp.get(), &TcpMgr::sig_sync_message_rsp,
		this, &ChatSyncManager::slot_sync_message_rsp);
	connect(tcp.get(), &TcpMgr::sig_load_chat_thread,
		this, &ChatSyncManager::slot_load_chat_thread);
	connect(tcp.get(), &TcpMgr::sig_connection_closed,
		this, &ChatSyncManager::slot_connection_closed);

	auto store = LocalChatStore::GetInstance();
	connect(store.get(), &LocalChatStore::sig_sync_state_loaded,
		this, &ChatSyncManager::slot_sync_state_loaded);
	connect(store.get(), &LocalChatStore::sig_conversations_upserted,
		this, &ChatSyncManager::slot_conversations_upserted);
	connect(store.get(), &LocalChatStore::sig_snapshot_applied,
		this, &ChatSyncManager::slot_snapshot_applied);
	connect(store.get(), &LocalChatStore::sig_bootstrap_marked,
		this, &ChatSyncManager::slot_bootstrap_marked);
	connect(store.get(), &LocalChatStore::sig_sync_page_applied,
		this, &ChatSyncManager::slot_sync_page_applied);

	_periodic_timer->setSingleShot(true);
	connect(_periodic_timer, &QTimer::timeout, this, &ChatSyncManager::slot_periodic_sync);
}

ChatSyncManager::~ChatSyncManager() {
}

QList<LocalFriendRequestDTO> ChatSyncManager::parseSnapshotRequests(
	const QJsonArray& array) {
	QList<LocalFriendRequestDTO> result;
	for (const QJsonValue& value : array) {
		if (!value.isObject()) continue;
		const QJsonObject object = value.toObject();
		LocalFriendRequestDTO request;
		request.friend_request_id = JsonId(object.value(QStringLiteral("friend_request_id")));
		request.requester_user_id = object.value(QStringLiteral("requester_user_id")).toInt();
		request.target_user_id = object.value(QStringLiteral("target_user_id")).toInt();
		request.request_message = object.value(QStringLiteral("request_message")).toString();
		request.status = object.value(QStringLiteral("status")).toInt(-1);
		request.thread_id = JsonId(object.value(QStringLiteral("thread_id")));
		request.peer_username = object.value(QStringLiteral("peer_username")).toString();
		request.peer_nickname = object.value(QStringLiteral("peer_nickname")).toString();
		request.peer_avatar_key = object.value(QStringLiteral("peer_avatar_key")).toString();
		request.peer_gender = object.value(QStringLiteral("peer_gender")).toInt();
		if (request.friend_request_id > 0 && request.requester_user_id > 0
			&& request.target_user_id > 0 && request.status >= 0 && request.status <= 2) {
			result.append(request);
		}
	}
	return result;
}

QList<LocalContactDTO> ChatSyncManager::parseSnapshotContacts(const QJsonArray& array) {
	QList<LocalContactDTO> result;
	for (const QJsonValue& value : array) {
		if (!value.isObject()) continue;
		const QJsonObject object = value.toObject();
		LocalContactDTO contact;
		contact.user_id = object.value(QStringLiteral("user_id")).toInt();
		contact.thread_id = JsonId(object.value(QStringLiteral("thread_id")));
		contact.username = object.value(QStringLiteral("username")).toString();
		contact.nickname = object.value(QStringLiteral("nickname")).toString();
		contact.avatar_key = object.value(QStringLiteral("avatar_key")).toString();
		contact.gender = object.value(QStringLiteral("gender")).toInt();
		if (contact.user_id > 0 && contact.thread_id > 0) result.append(contact);
	}
	return result;
}

void ChatSyncManager::slot_login_snapshot(QJsonObject snapshot, bool initialLogin) {
	Q_UNUSED(initialLogin);
	_checkpoint = JsonId(snapshot.value(QStringLiteral("checkpoint")));
	_snapshot_requests = parseSnapshotRequests(snapshot.value(QStringLiteral("apply_list")).toArray());
	_snapshot_contacts = parseSnapshotContacts(snapshot.value(QStringLiteral("friend_list")).toArray());
}

void ChatSyncManager::slot_start() {
	_state = SYNC_WAIT_STATE;
	_has_more_pending = false;
	_pull_pending = false;
	_applying_live = false;
	_pending_live.clear();
	LocalChatStore::GetInstance()->getSyncState();
	schedulePeriodicSync();
}

void ChatSyncManager::slot_sync_state_loaded(bool ok, qint64 lastEventSeq,
	bool bootstrapComplete) {
	if (_state != SYNC_WAIT_STATE) return;
	if (!ok) {
		_state = SYNC_IDLE;
		return;
	}
	_last_event_seq = lastEventSeq;
	if (bootstrapComplete) {
		_mark_after_snapshot = false;
		_state = SYNC_WAIT_SNAPSHOT;
		LocalChatStore::GetInstance()->applySnapshot(
			_snapshot_requests, _snapshot_contacts, false);
		return;
	}
	_bootstrap_conversations.clear();
	_state = SYNC_BOOTSTRAP_THREADS;
	sendThreadListRequest(0);
}

void ChatSyncManager::slot_load_chat_thread(bool loadMore, qint64 nextLastId,
	std::vector<std::shared_ptr<ChatThreadInfo> > chatThreads) {
	if (_state != SYNC_BOOTSTRAP_THREADS) return;
	const qint64 selfUserId = UserMgr::GetInstance()->GetUid();
	for (const auto& thread : chatThreads) {
		if (!thread) continue;
		LocalConversationDTO conversation;
		conversation.thread_id = thread->_thread_id;
		conversation.peer_user_id = selfUserId == thread->_lower_user_id
			? thread->_higher_user_id : thread->_lower_user_id;
		conversation.last_message_id = thread->_last_msg_id;
		conversation.updated_at = QDateTime::currentMSecsSinceEpoch();
		_bootstrap_conversations.append(conversation);
	}
	if (loadMore) {
		sendThreadListRequest(nextLastId);
		return;
	}
	_state = SYNC_WAIT_UPSERT;
	LocalChatStore::GetInstance()->upsertConversations(_bootstrap_conversations);
}

void ChatSyncManager::slot_conversations_upserted(bool ok) {
	if (_state != SYNC_WAIT_UPSERT) return;
	if (!ok) {
		_state = SYNC_IDLE;
		return;
	}
	_mark_after_snapshot = true;
	_state = SYNC_WAIT_SNAPSHOT;
	LocalChatStore::GetInstance()->applySnapshot(
		_snapshot_requests, _snapshot_contacts, true);
}

void ChatSyncManager::slot_snapshot_applied(bool ok) {
	if (_state != SYNC_WAIT_SNAPSHOT) return;
	if (!ok) {
		_state = SYNC_IDLE;
		return;
	}
	if (_mark_after_snapshot) {
		_state = SYNC_WAIT_MARK;
		LocalChatStore::GetInstance()->markBootstrapComplete(_checkpoint);
		return;
	}
	sendSyncRequest();
}

void ChatSyncManager::slot_bootstrap_marked(bool ok, qint64 checkpoint) {
	if (_state != SYNC_WAIT_MARK) return;
	if (!ok) {
		_state = SYNC_IDLE;
		return;
	}
	_last_event_seq = checkpoint;
	sendSyncRequest();
}

void ChatSyncManager::slot_user_message_notify(QJsonObject envelope) {
	const qint64 eventSeq = JsonId(envelope.value(QStringLiteral("event_seq")));
	if (eventSeq <= _last_event_seq) return;
	_pending_live.insert(eventSeq, envelope);
	if (_state == SYNC_IDLE) applyNextLiveIfPossible();
}

void ChatSyncManager::applyNextLiveIfPossible() {
	while (!_pending_live.isEmpty() && _pending_live.firstKey() <= _last_event_seq) {
		_pending_live.erase(_pending_live.begin());
	}
	const qint64 next = _last_event_seq + 1;
	if (_pending_live.contains(next)) {
		const QJsonObject envelope = _pending_live.take(next);
		_applying_live = true;
		_state = SYNC_WAIT_PAGE;
		applyEnvelopePage(QList<QJsonObject>() << envelope, next);
		return;
	}
	if (!_pending_live.isEmpty()) sendSyncRequest();
}

bool ChatSyncManager::parseEnvelope(const QJsonObject& envelope,
	UserEventDTO* event, LocalMessageDTO* message,
	LocalMessageResourceDTO* resource,
	LocalFriendRequestDTO* friendRequest, LocalContactDTO* contact) {
	if (!event || !message || !resource || !friendRequest || !contact) return false;
	*event = UserEventDTO();
	*message = LocalMessageDTO();
	*resource = LocalMessageResourceDTO();
	*friendRequest = LocalFriendRequestDTO();
	*contact = LocalContactDTO();
	event->event_seq = JsonId(envelope.value(QStringLiteral("event_seq")));
	event->event_type = envelope.value(QStringLiteral("event_type")).toInt(-1);
	if (event->event_seq <= 0) return false;

	if (event->event_type == 0 || event->event_type == 1 || event->event_type == 3) {
		event->message_id = JsonId(envelope.value(QStringLiteral("message_id")));
		message->message_id = event->message_id;
		message->thread_id = JsonId(envelope.value(QStringLiteral("thread_id")));
		message->sender_user_id = envelope.value(QStringLiteral("sender_user_id")).toInt();
		message->message_type = envelope.value(QStringLiteral("message_type")).toInt(-1);
		message->text_content = envelope.value(QStringLiteral("text_content")).toString();
		message->send_status = LOCAL_SEND_SENT;
		message->created_at = ServerTimeToEpoch(envelope.value(QStringLiteral("created_at")));
		if (message->created_at <= 0) message->created_at = QDateTime::currentMSecsSinceEpoch();
		if (message->message_id <= 0 || message->thread_id <= 0
			|| message->sender_user_id <= 0 || message->message_type != event->event_type) return false;
		if (message->message_type == 0) return envelope.contains(QStringLiteral("text_content"));
		resource->original_file_name = envelope.value(QStringLiteral("original_file_name")).toString();
		resource->file_size_bytes = JsonId(envelope.value(QStringLiteral("file_size_bytes")));
		resource->sha256 = envelope.value(QStringLiteral("sha256")).toString();
		resource->mime_type = envelope.value(QStringLiteral("mime_type")).toString();
		return !resource->original_file_name.isEmpty() && resource->file_size_bytes > 0
			&& resource->sha256.size() == 64 && !resource->mime_type.isEmpty();
	}

	if (event->event_type != 10 && event->event_type != 11 && event->event_type != 12) {
		return false;
	}
	event->friend_request_id = JsonId(envelope.value(QStringLiteral("friend_request_id")));
	friendRequest->friend_request_id = event->friend_request_id;
	friendRequest->requester_user_id = envelope.value(QStringLiteral("requester_user_id")).toInt();
	friendRequest->target_user_id = envelope.value(QStringLiteral("target_user_id")).toInt();
	friendRequest->request_message = envelope.value(QStringLiteral("request_message")).toString();
	friendRequest->status = envelope.value(QStringLiteral("status")).toInt(-1);
	friendRequest->thread_id = JsonId(envelope.value(QStringLiteral("thread_id")));
	friendRequest->peer_username = envelope.value(QStringLiteral("peer_username")).toString();
	friendRequest->peer_nickname = envelope.value(QStringLiteral("peer_nickname")).toString();
	friendRequest->peer_avatar_key = envelope.value(QStringLiteral("peer_avatar_key")).toString();
	friendRequest->peer_gender = envelope.value(QStringLiteral("peer_gender")).toInt();
	if (friendRequest->friend_request_id <= 0 || friendRequest->requester_user_id <= 0
		|| friendRequest->target_user_id <= 0 || friendRequest->status < 0
		|| friendRequest->status > 2) return false;
	if (event->event_type == 11) {
		contact->user_id = friendRequest->requester_user_id == UserMgr::GetInstance()->GetUid()
			? friendRequest->target_user_id : friendRequest->requester_user_id;
		contact->thread_id = friendRequest->thread_id;
		contact->username = friendRequest->peer_username;
		contact->nickname = friendRequest->peer_nickname;
		contact->avatar_key = friendRequest->peer_avatar_key;
		contact->gender = friendRequest->peer_gender;
		if (contact->thread_id <= 0) return false;
	}
	return true;
}

void ChatSyncManager::applyEnvelopePage(const QList<QJsonObject>& envelopes,
	qint64 nextEventSeq) {
	QList<UserEventDTO> events;
	QList<LocalMessageDTO> messages;
	QList<LocalMessageResourceDTO> resources;
	QList<LocalFriendRequestDTO> friendRequests;
	QList<LocalContactDTO> contacts;
	qint64 expected = _last_event_seq + 1;
	for (const QJsonObject& envelope : envelopes) {
		UserEventDTO event;
		LocalMessageDTO message;
		LocalMessageResourceDTO resource;
		LocalFriendRequestDTO friendRequest;
		LocalContactDTO contact;
		if (!parseEnvelope(envelope, &event, &message, &resource, &friendRequest, &contact)
			|| event.event_seq != expected++) {
			_state = SYNC_IDLE;
			_applying_live = false;
			return;
		}
		events.append(event);
		if (event.message_id > 0) {
			messages.append(message);
			resources.append(resource);
		} else {
			friendRequests.append(friendRequest);
			if (event.event_type == 11) contacts.append(contact);
		}
	}
	LocalChatStore::GetInstance()->applySyncPage(events, messages, resources,
		friendRequests, contacts, nextEventSeq);
}

void ChatSyncManager::slot_sync_message_rsp(QJsonObject response) {
	if (_state != SYNC_WAIT_PAGE || _applying_live) return;
	if (response.value(QStringLiteral("error")).toInt(ErrorCodes::ERR_JSON)
		!= ErrorCodes::SUCCESS) {
		_state = SYNC_IDLE;
		return;
	}
	_has_more_pending = response.value(QStringLiteral("has_more")).toBool();
	QList<QJsonObject> envelopes;
	for (const QJsonValue& value : response.value(QStringLiteral("events")).toArray()) {
		if (!value.isObject()) {
			_state = SYNC_IDLE;
			return;
		}
		envelopes.append(value.toObject());
	}
	const qint64 next = JsonId(response.value(QStringLiteral("next_event_seq")));
	const qint64 expected = envelopes.isEmpty()
		? _last_event_seq : JsonId(envelopes.last().value(QStringLiteral("event_seq")));
	if (next != expected || (_has_more_pending && envelopes.isEmpty())) {
		_state = SYNC_IDLE;
		return;
	}
	applyEnvelopePage(envelopes, next);
}

void ChatSyncManager::slot_sync_page_applied(bool ok, qint64 newEventSeq,
	QList<UserEventDTO> events, QList<LocalMessageDTO> messages,
	QList<LocalMessageResourceDTO> resources,
	QList<LocalFriendRequestDTO> friendRequests,
	QList<LocalContactDTO> contacts, QList<qint64> insertedMessageIds) {
	Q_UNUSED(messages);
	Q_UNUSED(resources);
	Q_UNUSED(insertedMessageIds);
	if (_state != SYNC_WAIT_PAGE) return;
	if (!ok) {
		_state = SYNC_IDLE;
		_applying_live = false;
		return;
	}
	_last_event_seq = newEventSeq;
	for (const UserEventDTO& event : events) {
		if (event.friend_request_id <= 0) continue;
		LocalFriendRequestDTO request;
		for (const LocalFriendRequestDTO& candidate : friendRequests) {
			if (candidate.friend_request_id == event.friend_request_id) {
				request = candidate;
				break;
			}
		}
		if (event.event_type == static_cast<int>(UserEventType::FRIEND_APPLY)
			&& request.target_user_id == UserMgr::GetInstance()->GetUid()) {
			auto apply = std::make_shared<AddFriendApply>(
				static_cast<int>(request.requester_user_id), request.peer_username,
				request.request_message, request.peer_avatar_key, request.peer_nickname,
				request.peer_gender, request.friend_request_id);
			UserMgr::GetInstance()->AddApplyList(std::make_shared<ApplyInfo>(apply));
			emit TcpMgr::GetInstance()->sig_friend_apply(apply);
		} else if (event.event_type == static_cast<int>(UserEventType::FRIEND_ACCEPT)
			|| event.event_type == static_cast<int>(UserEventType::FRIEND_REJECT)) {
			const qint64 peerUserId = request.requester_user_id == UserMgr::GetInstance()->GetUid()
				? request.target_user_id : request.requester_user_id;
			const FriendRequestStatus status = event.event_type ==
				static_cast<int>(UserEventType::FRIEND_ACCEPT)
				? FriendRequestStatus::ACCEPTED : FriendRequestStatus::REJECTED;
			const QList<qint64> handled = UserMgr::GetInstance()->UpdatePendingApplyStatusByPeer(
				static_cast<int>(peerUserId), status);
			for (qint64 requestId : handled) {
				emit TcpMgr::GetInstance()->sig_friend_request_handled(
					requestId, static_cast<int>(status));
			}
			if (status == FriendRequestStatus::ACCEPTED) {
				for (const LocalContactDTO& contact : contacts) {
					if (contact.user_id != peerUserId) continue;
					auto auth = std::make_shared<AuthInfo>(static_cast<int>(contact.user_id),
						contact.username, contact.nickname, contact.avatar_key, contact.gender);
					auth->_thread_id = contact.thread_id;
					emit TcpMgr::GetInstance()->sig_add_auth_friend(auth);
					break;
				}
			}
		}
	}

	const bool wasLive = _applying_live;
	_applying_live = false;
	if (!wasLive && _has_more_pending) {
		_has_more_pending = false;
		sendSyncRequest();
		return;
	}
	_state = SYNC_IDLE;
	if (!_pending_live.isEmpty()) {
		applyNextLiveIfPossible();
		return;
	}
	if (_pull_pending) {
		_pull_pending = false;
		sendSyncRequest();
	}
}

void ChatSyncManager::sendSyncRequest() {
	QJsonObject request;
	request[QStringLiteral("after_event_seq")] = QString::number(_last_event_seq);
	request[QStringLiteral("limit")] = 100;
	_state = SYNC_WAIT_PAGE;
	_applying_live = false;
	emit TcpMgr::GetInstance()->sig_send_data(ID_SYNC_USER_MESSAGE_REQ,
		QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatSyncManager::sendThreadListRequest(qint64 lastThreadId) {
	QJsonObject request;
	request[QStringLiteral("thread_id")] = QString::number(lastThreadId);
	emit TcpMgr::GetInstance()->sig_send_data(ID_LOAD_CHAT_THREAD_REQ,
		QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatSyncManager::schedulePeriodicSync() {
	_periodic_timer->start(QRandomGenerator::global()->bounded(30000, 60001));
}

void ChatSyncManager::slot_periodic_sync() {
	if (_state == SYNC_IDLE) sendSyncRequest();
	else _pull_pending = true;
	schedulePeriodicSync();
}

void ChatSyncManager::slot_connection_closed() {
	_state = SYNC_IDLE;
	_pending_live.clear();
	_periodic_timer->stop();
}
