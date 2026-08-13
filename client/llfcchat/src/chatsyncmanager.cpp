#include "chatsyncmanager.h"
#include "tcpmgr.h"
#include "localchatstore.h"
#include "usermgr.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QDateTime>

ChatSyncManager::ChatSyncManager()
    : _state(SYNC_IDLE), _last_sync_seq(0), _checkpoint(0),
      _has_more_pending(false), _pull_pending(false)
{
    //线程边界：公有 API → TCP 线程 slot
    connect(this, &ChatSyncManager::sig_start, this, &ChatSyncManager::slot_start);
    connect(this, &ChatSyncManager::sig_notify_online_message,
        this, &ChatSyncManager::slot_notify_online_message);
    //TcpMgr 事件转发（同线程 direct）
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_chat_login_ready,
        this, &ChatSyncManager::slot_start);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_sync_message_rsp,
        this, &ChatSyncManager::slot_sync_message_rsp);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_load_chat_thread,
        this, &ChatSyncManager::slot_load_chat_thread);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_connection_closed,
        this, &ChatSyncManager::slot_connection_closed);
    //本地库结果信号（worker 线程 → queued 到 TCP 线程）
    auto store = LocalChatStore::GetInstance();
    connect(store.get(), &LocalChatStore::sig_sync_state_loaded,
        this, &ChatSyncManager::slot_sync_state_loaded);
    connect(store.get(), &LocalChatStore::sig_conversations_upserted,
        this, &ChatSyncManager::slot_conversations_upserted);
    connect(store.get(), &LocalChatStore::sig_bootstrap_marked,
        this, &ChatSyncManager::slot_bootstrap_marked);
    connect(store.get(), &LocalChatStore::sig_sync_page_applied,
        this, &ChatSyncManager::slot_sync_page_applied);
    //1019/1039 落库成功后兜底拉取（实时推送的可靠性兜底，拉取中会合并为一次补拉）
    connect(store.get(), &LocalChatStore::sig_incoming_inserted,
        this, [this](bool ok, QList<LocalMessageDTO>, QList<qint64> insertedIds) {
            if (ok && !insertedIds.isEmpty()) {
                notifyOnlineMessage();
            }
        });
}

ChatSyncManager::~ChatSyncManager()
{
}

void ChatSyncManager::start()
{
    emit sig_start();
}

void ChatSyncManager::notifyOnlineMessage()
{
    emit sig_notify_online_message();
}

void ChatSyncManager::slot_start()
{
    //每次登录/重连都回读 sync_state（内存游标以 DB 为准）
    _state = SYNC_WAIT_STATE;
    _pull_pending = false;
    _has_more_pending = false;
    LocalChatStore::GetInstance()->getSyncState();
}

void ChatSyncManager::slot_notify_online_message()
{
    if (_state == SYNC_IDLE) {
        sendSyncRequest();
        return;
    }
    //拉取中/未就绪：合并为一次补拉
    _pull_pending = true;
}

void ChatSyncManager::slot_sync_state_loaded(bool ok, qint64 lastSyncSeq,
    bool bootstrapComplete)
{
    if (_state != SYNC_WAIT_STATE) {
        return;
    }
    if (!ok) {
        qWarning() << "[Sync] getSyncState failed";
        _state = SYNC_IDLE;
        return;
    }
    if (bootstrapComplete) {
        //已完成 bootstrap：直接进入增量
        _last_sync_seq = lastSyncSeq;
        sendSyncRequest();
        return;
    }
    //首启 bootstrap：取 checkpoint
    QJsonObject obj;
    obj["uid"] = UserMgr::GetInstance()->GetUid();
    obj["bootstrap"] = true;
    _state = SYNC_WAIT_BOOTSTRAP;
    emit TcpMgr::GetInstance()->sig_send_data(ID_SYNC_MESSAGE_REQ,
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void ChatSyncManager::slot_sync_message_rsp(QJsonObject rsp)
{
    int err = rsp["error"].toInt();
    if (err != ErrorCodes::SUCCESS) {
        qWarning() << "[Sync] sync rsp error" << err;
        _state = SYNC_IDLE;
        return;
    }

    if (_state == SYNC_WAIT_BOOTSTRAP) {
        //bootstrap 回包只带 checkpoint（十进制字符串）
        _checkpoint = rsp["checkpoint"].toString().toLongLong();
        _bootstrap_convs.clear();
        //走 1025/1026 拿会话列表
        sendThreadListRequest(0);
        _state = SYNC_BOOTSTRAP_THREADS;
        return;
    }

    if (_state != SYNC_WAIT_PAGE) {
        return;
    }

    //增量回包：整页交 applySyncPage，提交成功后才推进游标
    qint64 next_sync_seq = rsp["next_sync_seq"].toString().toLongLong();
    _has_more_pending = rsp["has_more"].toBool();
    QList<LocalMessageDTO> msgs;
    for (const QJsonValue& v : rsp["messages"].toArray()) {
        msgs.append(envelopeToDto(v.toObject()));
    }
    LocalChatStore::GetInstance()->applySyncPage(msgs, next_sync_seq);
}

void ChatSyncManager::slot_load_chat_thread(bool load_more, qint64 next_last_id,
    std::vector<std::shared_ptr<ChatThreadInfo>> chat_threads)
{
    if (_state != SYNC_BOOTSTRAP_THREADS) {
        return;
    }
    auto uid = UserMgr::GetInstance()->GetUid();
    for (auto& cti : chat_threads) {
        //先处理单聊，群聊跳过，以后添加
        if (cti->_type == "group") {
            continue;
        }
        LocalConversationDTO conv;
        conv.thread_id = cti->_thread_id;
        conv.peer_uid = (uid == cti->_user1_id) ? cti->_user2_id : cti->_user1_id;
        conv.last_server_message_id = cti->_last_msg_id;
        _bootstrap_convs.append(conv);
    }
    if (load_more) {
        sendThreadListRequest(next_last_id);
        return;
    }
    //会话列表拉全：写库后标记 bootstrap 完成
    LocalChatStore::GetInstance()->upsertConversations(_bootstrap_convs);
    _state = SYNC_WAIT_UPSERT;
}

void ChatSyncManager::slot_conversations_upserted(bool ok)
{
    if (_state != SYNC_WAIT_UPSERT) {
        return;
    }
    if (!ok) {
        qWarning() << "[Sync] upsert conversations failed";
        _state = SYNC_IDLE;
        return;
    }
    LocalChatStore::GetInstance()->markBootstrapComplete(_checkpoint);
    _state = SYNC_WAIT_MARK;
}

void ChatSyncManager::slot_bootstrap_marked(bool ok, qint64 checkpoint)
{
    if (_state != SYNC_WAIT_MARK) {
        return;
    }
    if (!ok) {
        qWarning() << "[Sync] mark bootstrap failed";
        _state = SYNC_IDLE;
        return;
    }
    //bootstrap 完成：从 checkpoint 进入增量同步
    _last_sync_seq = checkpoint;
    sendSyncRequest();
}

void ChatSyncManager::slot_sync_page_applied(bool ok, qint64 newSyncSeq,
    QList<LocalMessageDTO> msgs, QList<qint64> insertedIds)
{
    Q_UNUSED(msgs);
    Q_UNUSED(insertedIds);
    if (_state != SYNC_WAIT_PAGE) {
        return;
    }
    if (!ok) {
        //提交失败整体回滚游标不动，等下次触发重拉
        qWarning() << "[Sync] apply sync page failed, seq stays" << _last_sync_seq;
        _state = SYNC_IDLE;
        return;
    }
    _last_sync_seq = newSyncSeq;
    if (_has_more_pending) {
        //has_more 连拉下一页
        _has_more_pending = false;
        sendSyncRequest();
        return;
    }
    _state = SYNC_IDLE;
    if (_pull_pending) {
        _pull_pending = false;
        sendSyncRequest();
    }
}

void ChatSyncManager::slot_connection_closed()
{
    //断线：状态复位，重连后 slot_start 重新驱动
    _state = SYNC_IDLE;
    _has_more_pending = false;
    _pull_pending = false;
}

void ChatSyncManager::sendSyncRequest()
{
    QJsonObject obj;
    obj["uid"] = UserMgr::GetInstance()->GetUid();
    //after_sync_seq 十进制字符串
    obj["after_sync_seq"] = QString::number(_last_sync_seq);
    obj["limit"] = 100;
    _state = SYNC_WAIT_PAGE;
    emit TcpMgr::GetInstance()->sig_send_data(ID_SYNC_MESSAGE_REQ,
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void ChatSyncManager::sendThreadListRequest(qint64 lastThreadId)
{
    QJsonObject obj;
    obj["uid"] = UserMgr::GetInstance()->GetUid();
    obj["thread_id"] = lastThreadId;
    emit TcpMgr::GetInstance()->sig_send_data(ID_LOAD_CHAT_THREAD_REQ,
        QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

LocalMessageDTO ChatSyncManager::envelopeToDto(const QJsonObject& envelope)
{
    LocalMessageDTO dto;
    //message_id/thread_id/sync_seq 均为十进制字符串；uid 保持数字
    dto.server_message_id = envelope["message_id"].toString().toLongLong();
    dto.thread_id = envelope["thread_id"].toString().toLongLong();
    dto.client_message_id = envelope["unique_id"].toString();
    dto.sender_id = envelope["fromuid"].toInt();
    dto.receiver_id = envelope["touid"].toInt();
    dto.message_type = envelope["msg_type"].toInt(static_cast<int>(ChatMsgType::TEXT));
    dto.content = envelope["content"].toString();
    dto.content_size = envelope["content_size"].toString();
    dto.created_at = envelope["chat_time"].toString();
    dto.send_state = SEND_STATE_SENT;
    return dto;
}
