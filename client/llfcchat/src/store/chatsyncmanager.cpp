#include "chatsyncmanager.h"

#include "global.h"
#include "localchatdb.h"
#include "localchatstore.h"
#include "tcpmgr.h"
#include "usermgr.h"

#include <QJsonDocument>
#include <QRandomGenerator>

ChatSyncManager::ChatSyncManager()
	: _state(SYNC_IDLE), _last_recv_seq(0), _checkpoint(0),
	  _has_more_pending(false), _pull_pending(false), _applying_live(false),
	  _mark_after_snapshot(false)
{
	auto tcp = TcpMgr::GetInstance();
	connect(tcp.get(), &TcpMgr::sig_chat_login_ready, this, &ChatSyncManager::slot_start);
	connect(tcp.get(), &TcpMgr::sig_login_snapshot, this, &ChatSyncManager::slot_login_snapshot);
	connect(tcp.get(), &TcpMgr::sig_user_message_notify,
		this, &ChatSyncManager::slot_user_message_notify);
	connect(tcp.get(), &TcpMgr::sig_user_message_business_rsp,
		this, &ChatSyncManager::slot_business_response);
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

	_periodic_timer = new QTimer(this);
	_periodic_timer->setSingleShot(true);
	connect(_periodic_timer, &QTimer::timeout, this, &ChatSyncManager::slot_periodic_sync);
}

ChatSyncManager::~ChatSyncManager() {}

void ChatSyncManager::slot_login_snapshot(QJsonObject snapshot, bool initialLogin)
{
	Q_UNUSED(initialLogin);
	_checkpoint = snapshot["checkpoint"].toString().toLongLong();
	_snapshot_requests = snapshot["apply_list"].toArray();
	_snapshot_contacts = snapshot["friend_list"].toArray();
}

void ChatSyncManager::slot_start()
{
	_state = SYNC_WAIT_STATE;
	_has_more_pending = false;
	_pull_pending = false;
	_applying_live = false;
	_pending_live.clear();
	LocalChatStore::GetInstance()->getSyncState();
	schedulePeriodicSync();
}

void ChatSyncManager::slot_sync_state_loaded(bool ok, qint64 lastRecvSeq,
	bool bootstrapComplete)
{
	if (_state != SYNC_WAIT_STATE) return;
	if (!ok) { _state = SYNC_IDLE; return; }
	_last_recv_seq = lastRecvSeq;
	if (bootstrapComplete) {
		_mark_after_snapshot = false;
		_state = SYNC_WAIT_SNAPSHOT;
		LocalChatStore::GetInstance()->applySnapshot(
			_snapshot_requests, _snapshot_contacts, false);
		return;
	}
	_bootstrap_convs.clear();
	_state = SYNC_BOOTSTRAP_THREADS;
	sendThreadListRequest(0);
}

void ChatSyncManager::slot_load_chat_thread(bool loadMore, qint64 nextLastId,
	std::vector<std::shared_ptr<ChatThreadInfo> > chatThreads)
{
	if (_state != SYNC_BOOTSTRAP_THREADS) return;
	const qint64 uid = UserMgr::GetInstance()->GetUid();
	for (const auto& thread : chatThreads) {
		if (!thread || thread->_type == "group") continue;
		LocalConversationDTO conv;
		conv.thread_id = thread->_thread_id;
		conv.peer_uid = uid == thread->_user1_id ? thread->_user2_id : thread->_user1_id;
		conv.last_server_message_id = thread->_last_msg_id;
		_bootstrap_convs.append(conv);
	}
	if (loadMore) { sendThreadListRequest(nextLastId); return; }
	_state = SYNC_WAIT_UPSERT;
	LocalChatStore::GetInstance()->upsertConversations(_bootstrap_convs);
}

void ChatSyncManager::slot_conversations_upserted(bool ok)
{
	if (_state != SYNC_WAIT_UPSERT) return;
	if (!ok) { _state = SYNC_IDLE; return; }
	_mark_after_snapshot = true;
	_state = SYNC_WAIT_SNAPSHOT;
	LocalChatStore::GetInstance()->applySnapshot(
		_snapshot_requests, _snapshot_contacts, true);
}

void ChatSyncManager::slot_snapshot_applied(bool ok)
{
	if (_state != SYNC_WAIT_SNAPSHOT) return;
	if (!ok) { _state = SYNC_IDLE; return; }
	if (_mark_after_snapshot) {
		_state = SYNC_WAIT_MARK;
		LocalChatStore::GetInstance()->markBootstrapComplete(_checkpoint);
		return;
	}
	sendSyncRequest();
}

void ChatSyncManager::slot_bootstrap_marked(bool ok, qint64 checkpoint)
{
	if (_state != SYNC_WAIT_MARK) return;
	if (!ok) { _state = SYNC_IDLE; return; }
	_last_recv_seq = checkpoint;
	sendSyncRequest();
}

void ChatSyncManager::slot_user_message_notify(QJsonObject envelope)
{
	bool valid_seq = false;
	const qint64 seq = envelope["recv_seq"].toString().toLongLong(&valid_seq);
	if (!envelope["recv_seq"].isString() || !valid_seq ||
		seq <= 0 || seq <= _last_recv_seq) return;
	_pending_live.insert(seq, envelope);
	if (_state == SYNC_IDLE) applyNextLiveIfPossible();
}

void ChatSyncManager::slot_business_response(QJsonObject envelope)
{
	if (envelope["error"].toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS ||
		!envelope.contains("message_id")) return;
	LocalMessageDTO dto = envelopeToDto(envelope);
	//业务响应中的 recv_seq 属于消息接收方，不推进当前发起方游标。
	dto.recv_seq = 0;
	LocalChatStore::GetInstance()->insertIncoming(QList<LocalMessageDTO>() << dto);
}

void ChatSyncManager::applyNextLiveIfPossible()
{
	while (!_pending_live.isEmpty() && _pending_live.firstKey() <= _last_recv_seq) {
		_pending_live.erase(_pending_live.begin());
	}
	const qint64 next = _last_recv_seq + 1;
	if (_pending_live.contains(next)) {
		const LocalMessageDTO dto = envelopeToDto(_pending_live.take(next));
		_applying_live = true;
		_state = SYNC_WAIT_PAGE;
		LocalChatStore::GetInstance()->applySyncPage(QList<LocalMessageDTO>() << dto, next);
		return;
	}
	if (!_pending_live.isEmpty()) sendSyncRequest();
}

void ChatSyncManager::slot_sync_message_rsp(QJsonObject rsp)
{
	if (_state != SYNC_WAIT_PAGE || _applying_live) return;
	if (rsp["error"].toInt(ErrorCodes::ERR_JSON) != ErrorCodes::SUCCESS) {
		_state = SYNC_IDLE;
		return;
	}
	_has_more_pending = rsp["has_more"].toBool();
	QList<LocalMessageDTO> messages;
	qint64 expected_seq = _last_recv_seq + 1;
	for (const QJsonValue& value : rsp["messages"].toArray()) {
		if (!value.isObject() || !value.toObject()["recv_seq"].isString()) {
			_state = SYNC_IDLE;
			return;
		}
		const LocalMessageDTO dto = envelopeToDto(value.toObject());
		if (dto.recv_seq != expected_seq) {
			_state = SYNC_IDLE;
			return;
		}
		messages.append(dto);
		++expected_seq;
	}
	bool valid_next = false;
	const qint64 next = rsp["next_recv_seq"].toString().toLongLong(&valid_next);
	const qint64 expected_next = messages.isEmpty()
		? _last_recv_seq : messages.last().recv_seq;
	if (!rsp["next_recv_seq"].isString() || !valid_next || next != expected_next ||
		(_has_more_pending && messages.isEmpty())) {
		_state = SYNC_IDLE;
		return;
	}
	LocalChatStore::GetInstance()->applySyncPage(messages, next);
}

void ChatSyncManager::slot_sync_page_applied(bool ok, qint64 newRecvSeq,
	QList<LocalMessageDTO> messages, QList<qint64> insertedIds)
{
	if (_state != SYNC_WAIT_PAGE) return;
	if (!ok) { _state = SYNC_IDLE; _applying_live = false; return; }
	_last_recv_seq = newRecvSeq;
	for (const LocalMessageDTO& dto : messages) {
		const bool newly_applied = insertedIds.contains(dto.server_message_id);
		if (dto.message_type == static_cast<int>(ChatMsgType::FRIEND_APPLY) &&
			dto.receiver_id == UserMgr::GetInstance()->GetUid() && newly_applied) {
			auto apply = std::make_shared<AddFriendApply>(
				static_cast<int>(dto.sender_id), dto.sender_name, dto.content,
				dto.sender_icon, dto.sender_nick, dto.sender_sex, dto.server_message_id);
			UserMgr::GetInstance()->AddApplyList(std::make_shared<ApplyInfo>(apply));
			emit TcpMgr::GetInstance()->sig_friend_apply(apply);
		}
		else if (dto.message_type == static_cast<int>(ChatMsgType::FRIEND_ACCEPT) &&
			newly_applied) {
			const int peer_uid = static_cast<int>(dto.sender_id ==
				UserMgr::GetInstance()->GetUid() ? dto.receiver_id : dto.sender_id);
			const auto handled = UserMgr::GetInstance()->UpdatePendingApplyStatusByPeer(
				peer_uid, FriendRequestStatus::ACCEPTED);
			for (qint64 message_id : handled) {
				emit TcpMgr::GetInstance()->sig_friend_request_handled(
					message_id, static_cast<int>(FriendRequestStatus::ACCEPTED));
			}
			auto auth = std::make_shared<AuthInfo>(peer_uid,
				dto.sender_name, dto.sender_nick, dto.sender_icon, dto.sender_sex);
			auth->_thread_id = dto.thread_id;
			emit TcpMgr::GetInstance()->sig_add_auth_friend(auth);
		}
	}
	const bool was_live = _applying_live;
	_applying_live = false;
	if (!was_live && _has_more_pending) {
		_has_more_pending = false;
		sendSyncRequest();
		return;
	}
	_state = SYNC_IDLE;
	if (!_pending_live.isEmpty()) {
		applyNextLiveIfPossible();
		return;
	}
	if (_pull_pending) { _pull_pending = false; sendSyncRequest(); }
}

void ChatSyncManager::sendSyncRequest()
{
	QJsonObject request;
	request["after_recv_seq"] = QString::number(_last_recv_seq);
	request["limit"] = 100;
	_state = SYNC_WAIT_PAGE;
	_applying_live = false;
	emit TcpMgr::GetInstance()->sig_send_data(ID_SYNC_USER_MESSAGE_REQ,
		QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatSyncManager::sendThreadListRequest(qint64 lastThreadId)
{
	QJsonObject request;
	request["uid"] = UserMgr::GetInstance()->GetUid();
	request["thread_id"] = QString::number(lastThreadId);
	emit TcpMgr::GetInstance()->sig_send_data(ID_LOAD_CHAT_THREAD_REQ,
		QJsonDocument(request).toJson(QJsonDocument::Compact));
}

void ChatSyncManager::schedulePeriodicSync()
{
	_periodic_timer->start(QRandomGenerator::global()->bounded(30000, 60001));
}

void ChatSyncManager::slot_periodic_sync()
{
	if (_state == SYNC_IDLE) sendSyncRequest();
	else _pull_pending = true;
	schedulePeriodicSync();
}

void ChatSyncManager::slot_connection_closed()
{
	_state = SYNC_IDLE;
	_pending_live.clear();
	_periodic_timer->stop();
}

LocalMessageDTO ChatSyncManager::envelopeToDto(const QJsonObject& envelope)
{
	LocalMessageDTO dto;
	dto.server_message_id = envelope["message_id"].toString().toLongLong();
	dto.recv_seq = envelope["recv_seq"].toString().toLongLong();
	dto.sender_id = envelope["fromuid"].toInt();
	dto.receiver_id = envelope["touid"].toInt();
	dto.client_message_id = envelope["unique_id"].toString();
	if (dto.client_message_id.isEmpty()) dto.client_message_id =
		"server-" + QString::number(dto.server_message_id);
	else if (dto.sender_id != UserMgr::GetInstance()->GetUid()) {
		dto.client_message_id = QString::number(dto.sender_id) + ":" + dto.client_message_id;
	}
	dto.thread_id = envelope["thread_id"].toString().toLongLong();
	dto.message_type = envelope["msg_type"].toInt();
	dto.content = envelope["content"].toString();
	dto.content_size = envelope["content_size"].toString();
	dto.resource_status = envelope["resource_status"].toInt();
	dto.content_hash = envelope["content_hash"].toString();
	dto.mime_type = envelope["mime_type"].toString();
	dto.created_at = envelope["chat_time"].toString();
	dto.business_status = envelope["business_status"].toInt();
	dto.related_message_id = envelope["related_message_id"].toString().toLongLong();
	dto.handled_at = envelope["handled_at"].toString();
	dto.requester_remark = envelope.contains("contact_remark")
		? envelope["contact_remark"].toString()
		: envelope["requester_remark"].toString();
	const QJsonObject profile = envelope.contains("peer_profile")
		? envelope["peer_profile"].toObject() : envelope["sender_profile"].toObject();
	dto.sender_name = profile["name"].toString();
	dto.sender_nick = profile["nick"].toString();
	dto.sender_icon = profile["icon"].toString();
	dto.sender_desc = profile["desc"].toString();
	dto.sender_sex = profile["sex"].toInt();
	dto.send_state = SEND_STATE_SENT;
	return dto;
}
