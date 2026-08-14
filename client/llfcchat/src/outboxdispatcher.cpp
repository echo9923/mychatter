#include "outboxdispatcher.h"
#include "tcpmgr.h"
#include "filetcpmgr.h"
#include "localchatstore.h"
#include "usermgr.h"
#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

OutboxDispatcher::OutboxDispatcher()
    : _scan_timer(nullptr), _retry_initial_ms(2000), _retry_max_ms(30000), _connected(false)
{
    loadRetryConfig();
    //线程边界：公有 API → TCP 线程 slot
    connect(this, &OutboxDispatcher::sig_start, this, &OutboxDispatcher::slot_start);
    connect(this, &OutboxDispatcher::sig_notify_send_enqueued,
        this, &OutboxDispatcher::slot_notify_send_enqueued);
    //TcpMgr 事件转发（同线程 direct）
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_chat_login_ready,
        this, &OutboxDispatcher::slot_start);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_connection_closed,
        this, &OutboxDispatcher::slot_connection_closed);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_text_msg_rsp_forward,
        this, &OutboxDispatcher::slot_text_msg_rsp);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_resource_msg_meta_rsp_forward,
        this, &OutboxDispatcher::slot_resource_msg_meta_rsp);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_delivery_ack_rsp_forward,
        this, &OutboxDispatcher::slot_delivery_ack_rsp);
    //FileTcpMgr 上传事件（File 线程 → queued 到 TCP 线程）
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_resource_upload_done,
        this, &OutboxDispatcher::slot_resource_upload_done);
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_resource_upload_failed,
        this, &OutboxDispatcher::slot_resource_upload_failed);
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_upload_progress_rsp,
        this, &OutboxDispatcher::slot_upload_progress_rsp);
    //本地库结果信号（worker 线程 → queued 到 TCP 线程）
    auto store = LocalChatStore::GetInstance();
    connect(store.get(), &LocalChatStore::sig_outbox_loaded,
        this, &OutboxDispatcher::slot_outbox_loaded);
    connect(store.get(), &LocalChatStore::sig_message_loaded,
        this, &OutboxDispatcher::slot_message_loaded);
    connect(store.get(), &LocalChatStore::sig_incoming_inserted,
        this, &OutboxDispatcher::slot_incoming_inserted);
    //250ms 扫描定时器，parent 到 this，随 moveToThread 迁移到 TCP 线程
    _scan_timer = new QTimer(this);
    _scan_timer->setInterval(250);
    connect(_scan_timer, &QTimer::timeout, this, &OutboxDispatcher::slot_scan_timeout);
}

OutboxDispatcher::~OutboxDispatcher()
{
}

void OutboxDispatcher::loadRetryConfig()
{
    //沿用 main.cpp 读取方式：applicationDirPath/config.ini
    QString app_path = QCoreApplication::applicationDirPath();
    QString config_path = QDir::toNativeSeparators(app_path + QDir::separator() + "config.ini");
    QSettings settings(config_path, QSettings::IniFormat);

    bool ok = false;
    qint64 v = settings.value("Delivery/RequestRetryInitialMs", 2000).toLongLong(&ok);
    if (ok && v > 0) {
        _retry_initial_ms = v;
    }
    v = settings.value("Delivery/RequestRetryMaxMs", 30000).toLongLong(&ok);
    if (ok && v > 0) {
        _retry_max_ms = v;
    }
    qDebug() << "[Outbox] RequestRetryInitialMs=" << _retry_initial_ms
             << " RequestRetryMaxMs=" << _retry_max_ms;
}

void OutboxDispatcher::start()
{
    emit sig_start();
}

void OutboxDispatcher::notifySendEnqueued(const LocalMessageDTO& dto)
{
    emit sig_notify_send_enqueued(dto);
}

void OutboxDispatcher::slot_start()
{
    _connected = true;
    //重启/重连恢复：loadOutbox 结果到达后统一恢复 + 派发
    LocalChatStore::GetInstance()->loadOutbox();
    if (!_scan_timer->isActive()) {
        _scan_timer->start();
    }
}

void OutboxDispatcher::slot_notify_send_enqueued(LocalMessageDTO dto)
{
    Q_UNUSED(dto);
    //入库已成功，刷新内存镜像后由 slot_outbox_loaded 统一派发
    LocalChatStore::GetInstance()->loadOutbox();
    if (_connected && !_scan_timer->isActive()) {
        _scan_timer->start();
    }
}

void OutboxDispatcher::slot_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries)
{
    if (!ok) {
        return;
    }
    _entries.clear();
    for (const OutboxEntryDTO& entry : entries) {
        _entries.insert(entry.dedup_key, entry);
    }
    //重启恢复：uploading 阶段条目回查 server_message_id 后统一走 1041 对齐续传
    for (const OutboxEntryDTO& entry : entries) {
        if (entry.operation_type != OUTBOX_OP_SEND_RESOURCE
            || entry.stage != RESOURCE_STAGE_UPLOADING) {
            continue;
        }
        QJsonObject payload = QJsonDocument::fromJson(entry.payload.toUtf8()).object();
        QString name = payload["file_name"].toString();
        if (_uploading.contains(name) || _resume_pending.contains(entry.request_id)) {
            continue;
        }
        _resume_pending.insert(entry.request_id);
        LocalChatStore::GetInstance()->getMessageByClientId(entry.request_id);
    }
    dispatchDueEntries();
}

void OutboxDispatcher::slot_message_loaded(bool ok, LocalMessageDTO dto)
{
    if (!_resume_pending.contains(dto.client_message_id)) {
        return;
    }
    _resume_pending.remove(dto.client_message_id);
    if (!ok || dto.server_message_id <= 0) {
        //本地消息行缺失或无 server_message_id，无法续传，按失败收尾
        qWarning() << "[Outbox] resume upload failed, mark failed:" << dto.client_message_id;
        LocalChatStore::GetInstance()->markSendFailed(dto.client_message_id);
        removeEntry(dto.client_message_id);
        return;
    }
    auto iter = _entries.find(dto.client_message_id);
    if (iter == _entries.end()) {
        return;
    }
    startResourceUpload(iter.value(), dto.server_message_id);
}

void OutboxDispatcher::dispatchDueEntries()
{
    if (!_connected) {
        return;
    }
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    //DELIVERY_ACK 聚合为一个 1049 帧（message_ids 元素为十进制字符串）
    QJsonArray ack_ids;
    QStringList ack_keys;
    for (auto iter = _entries.begin(); iter != _entries.end(); ++iter) {
        OutboxEntryDTO& entry = iter.value();
        if (entry.operation_type != OUTBOX_OP_DELIVERY_ACK
            || entry.next_retry_at > now) {
            continue;
        }
        QJsonObject payload = QJsonDocument::fromJson(entry.payload.toUtf8()).object();
        ack_ids.append(payload["message_id"].toString());
        ack_keys.append(entry.dedup_key);
    }
    if (!ack_ids.isEmpty()) {
        QJsonObject obj;
        obj["uid"] = UserMgr::GetInstance()->GetUid();
        obj["message_ids"] = ack_ids;
        emit TcpMgr::GetInstance()->sig_send_data(ID_CHAT_DELIVERY_ACK_REQ,
            QJsonDocument(obj).toJson(QJsonDocument::Compact));
        for (const QString& key : ack_keys) {
            scheduleRetry(_entries[key]);
        }
        qDebug() << "[Outbox] sent ACK batch" << ack_keys.size();
    }

    for (auto iter = _entries.begin(); iter != _entries.end(); ++iter) {
        OutboxEntryDTO& entry = iter.value();
        if (entry.next_retry_at > now) {
            continue;
        }
        if (entry.operation_type == OUTBOX_OP_SEND_TEXT) {
            emit TcpMgr::GetInstance()->sig_send_data(ID_TEXT_CHAT_MSG_REQ,
                entry.payload.toUtf8());
            scheduleRetry(entry);
            continue;
        }
        if (entry.operation_type == OUTBOX_OP_SEND_RESOURCE) {
            dispatchResourceEntry(entry);
            continue;
        }
    }
}

void OutboxDispatcher::dispatchResourceEntry(const OutboxEntryDTO& entry)
{
    //uploading 阶段由 1042/1038 事件推进（或重启恢复续传），扫描不重发 1035
    if (entry.stage != RESOURCE_STAGE_METADATA) {
        return;
    }
    QJsonObject payload = QJsonDocument::fromJson(entry.payload.toUtf8()).object();
    QString local_path = payload["text_or_url"].toString();
    if (!QFile::exists(local_path)) {
        //源文件丢失：标 failed 并停止重试
        qWarning() << "[Outbox] resource source missing, mark failed:" << local_path;
        LocalChatStore::GetInstance()->markSendFailed(entry.request_id);
        removeEntry(entry.dedup_key);
        return;
    }
    emit TcpMgr::GetInstance()->sig_send_data(ID_CREATE_RESOURCE_MSG_REQ,
        entry.payload.toUtf8());
    scheduleRetry(_entries[entry.dedup_key]);
}

void OutboxDispatcher::startResourceUpload(const OutboxEntryDTO& entry, qint64 server_message_id)
{
    QJsonObject payload = QJsonDocument::fromJson(entry.payload.toUtf8()).object();
    QString local_path = payload["text_or_url"].toString();
    QString name = payload["file_name"].toString();
    if (!QFile::exists(local_path)) {
        //源文件丢失：标 failed 并停止重试
        qWarning() << "[Outbox] resource source missing, mark failed:" << local_path;
        LocalChatStore::GetInstance()->markSendFailed(entry.request_id);
        removeEntry(entry.dedup_key);
        return;
    }

    const int msg_type = payload["msg_type"].toInt(static_cast<int>(ChatMsgType::PIC));
    qint64 total_size = payload["content_size"].toString().toLongLong();
    QString content_hash = payload["content_hash"].toString();
    auto file_info = UserMgr::GetInstance()->GetTransFileByName(name);
    if (!file_info) {
        //重启恢复路径：GUI 未持有 MsgInfo，按 payload 重建（片哈希丢失时由
        //FileTcpMgr::BatchSend 读源文件重算）
        file_info = std::make_shared<MsgInfo>(
            msg_type == static_cast<int>(ChatMsgType::FILE) ? MsgType::FILE_MSG : MsgType::IMG_MSG,
            local_path, QPixmap(local_path), name, total_size, content_hash);
        UserMgr::GetInstance()->AddTransFile(name, file_info);
    }
    file_info->_msg_id = server_message_id;
    file_info->_total_size = total_size;
    file_info->_content_hash = content_hash;
    file_info->_max_seq = (total_size + MAX_FILE_LEN - 1) / MAX_FILE_LEN;
    file_info->_thread_id = payload["thread_id"].toVariant().toLongLong();
    file_info->_sender = payload["fromuid"].toInt();
    file_info->_receiver = payload["touid"].toInt();
    file_info->_transfer_type = TransferType::Upload;
    file_info->_transfer_state = TransferState::Uploading;
    _uploading.insert(name, entry.request_id);

    //首传/续传统一入口：1041 查服务端真实偏移（.part 长度），1042 回包对齐后窗口续发
    QJsonObject req;
    req["message_id"] = QString::number(server_message_id);
    FileTcpMgr::GetInstance()->SendData(ID_RESOURCE_UPLOAD_PROGRESS_REQ,
        QJsonDocument(req).toJson(QJsonDocument::Compact));
}

void OutboxDispatcher::resumeUploadByProgress(qint64 message_id, qint64 server_offset,
    int resource_status)
{
    auto file_info = UserMgr::GetInstance()->GetTransFileByMsgId(message_id);
    if (!file_info) {
        return;
    }
    const QString name = file_info->_unique_name;
    if (!_uploading.contains(name)) {
        return;
    }

    if (resource_status == RESOURCE_READY
        || (file_info->_total_size > 0 && server_offset >= file_info->_total_size)) {
        //服务端已收齐（重试路径幂等）：直接按完成收尾
        LocalChatStore::GetInstance()->confirmResourceSent(_uploading.value(name));
        removeEntry(_uploading.value(name));
        _uploading.remove(name);
        return;
    }

    //按服务端真值对齐确认偏移，从下一片续发
    qint64 confirmed = (file_info->_total_size > 0 && server_offset >= file_info->_total_size)
        ? file_info->_max_seq : server_offset / MAX_FILE_LEN;
    file_info->_rsp_seqs.clear();
    file_info->_flighting_seqs.clear();
    file_info->_last_confirmed_seq = confirmed;
    file_info->_seq = confirmed + 1;
    file_info->_rsp_size = qMin(server_offset, file_info->_total_size);
    file_info->_transfer_state = TransferState::Uploading;
    //BatchSend 读写 _cwnd_size（File 线程状态），跨线程统一投递
    FileTcpMgr::GetInstance()->PostBatchSend(file_info);
}

void OutboxDispatcher::scheduleRetry(OutboxEntryDTO& entry)
{
    //指数退避 2s→30s：每次派发后推进 retry_count 与下次时刻
    qint64 delay = _retry_initial_ms;
    for (int i = 0; i < entry.retry_count; ++i) {
        delay = qMin(delay * 2, _retry_max_ms);
    }
    entry.retry_count += 1;
    entry.next_retry_at = QDateTime::currentMSecsSinceEpoch() + delay;
    LocalChatStore::GetInstance()->updateOutboxRetry(entry.dedup_key, entry.retry_count,
        entry.next_retry_at);
}

void OutboxDispatcher::removeEntry(const QString& dedup_key)
{
    _entries.remove(dedup_key);
}

void OutboxDispatcher::failResourceByName(const QString& unique_name)
{
    auto iter = _uploading.find(unique_name);
    if (iter == _uploading.end()) {
        return;
    }
    const QString client_message_id = iter.value();
    _uploading.erase(iter);
    LocalChatStore::GetInstance()->markSendFailed(client_message_id);
    removeEntry(client_message_id);
}

void OutboxDispatcher::slot_scan_timeout()
{
    if (!_connected) {
        _scan_timer->stop();
        return;
    }
    dispatchDueEntries();
}

void OutboxDispatcher::slot_text_msg_rsp(int error, QString unique_id, qint64 message_id,
    QString chat_time)
{
    if (error == ErrorCodes::SUCCESS) {
        //1018 成功：确认入库 + 删 outbox
        LocalChatStore::GetInstance()->confirmTextSent(unique_id, message_id, chat_time);
        removeEntry(unique_id);
        return;
    }
    if (error == ErrorCodes::MESSAGE_CONFLICT) {
        //永久冲突：标 SEND_FAILED 停止重传
        LocalChatStore::GetInstance()->markSendFailed(unique_id);
        removeEntry(unique_id);
        return;
    }
    //transient（1014/1016）：更新退避等待扫描重试
    auto iter = _entries.find(unique_id);
    if (iter != _entries.end()) {
        scheduleRetry(iter.value());
    }
}

void OutboxDispatcher::slot_resource_msg_meta_rsp(int error, QString unique_id, QString file_name,
    qint64 message_id, qint64 thread_id, qint64 fromuid, qint64 touid)
{
    Q_UNUSED(file_name);
    Q_UNUSED(thread_id);
    Q_UNUSED(fromuid);
    Q_UNUSED(touid);
    auto iter = _entries.find(unique_id);
    //permanent：冲突/资源元数据非法/超限，标 SEND_FAILED 停止重传
    if (error == ErrorCodes::MESSAGE_CONFLICT
        || error == ErrorCodes::RESOURCE_INVALID
        || error == ErrorCodes::RESOURCE_SIZE_EXCEEDED) {
        LocalChatStore::GetInstance()->markSendFailed(unique_id);
        removeEntry(unique_id);
        return;
    }
    if (error != ErrorCodes::SUCCESS) {
        //transient（1014/1016）：更新退避等待扫描重试
        if (iter != _entries.end()) {
            scheduleRetry(iter.value());
        }
        return;
    }
    if (iter == _entries.end()) {
        return;
    }
    //1036 成功：推进 outbox 阶段 uploading（不删条目）并启动上传（1041 对齐）
    LocalChatStore::GetInstance()->updateResourceStage(unique_id, message_id,
        RESOURCE_STAGE_UPLOADING);
    iter.value().stage = RESOURCE_STAGE_UPLOADING;
    iter.value().next_retry_at = 0;
    startResourceUpload(iter.value(), message_id);
}

void OutboxDispatcher::slot_resource_upload_done(QString unique_name)
{
    auto iter = _uploading.find(unique_name);
    if (iter == _uploading.end()) {
        return;
    }
    QString client_message_id = iter.value();
    _uploading.erase(iter);
    //1038 resource_status=Ready：send_state=sent + 删 outbox
    LocalChatStore::GetInstance()->confirmResourceSent(client_message_id);
    removeEntry(client_message_id);
}

void OutboxDispatcher::slot_resource_upload_failed(QString unique_name, int error)
{
    qWarning() << "[Outbox] resource upload permanent failed:" << unique_name << "err=" << error;
    failResourceByName(unique_name);
}

void OutboxDispatcher::slot_upload_progress_rsp(qint64 message_id, int error,
    qint64 server_offset, int resource_status)
{
    if (error != ErrorCodes::SUCCESS) {
        //1022 消息不存在（DB 行缺失）等：按失败收尾；transient 错误交由重连恢复兜底
        if (error == ErrorCodes::MSG_ID_ERR) {
            auto file_info = UserMgr::GetInstance()->GetTransFileByMsgId(message_id);
            if (file_info) {
                failResourceByName(file_info->_unique_name);
            }
        }
        return;
    }
    resumeUploadByProgress(message_id, server_offset, resource_status);
}

void OutboxDispatcher::slot_delivery_ack_rsp(int error, QList<qint64> message_ids)
{
    if (error != ErrorCodes::SUCCESS) {
        //transient：保留条目等待扫描重发
        qDebug() << "[Outbox] ACK transient rsp error" << error << "keeping pending";
        return;
    }
    for (qint64 message_id : message_ids) {
        QString dedup_key = "ack_" + QString::number(message_id);
        removeEntry(dedup_key);
        LocalChatStore::GetInstance()->deleteOutboxEntry(dedup_key);
    }
}

void OutboxDispatcher::slot_incoming_inserted(bool ok, QList<LocalMessageDTO> msgs,
    QList<qint64> insertedIds)
{
    Q_UNUSED(msgs);
    if (!ok || insertedIds.isEmpty()) {
        return;
    }
    //新 DELIVERY_ACK 条目已落库，立即刷新镜像并派发 1049
    LocalChatStore::GetInstance()->loadOutbox();
}

void OutboxDispatcher::slot_connection_closed()
{
    _connected = false;
    _scan_timer->stop();
    //断线后上传状态失效，重连 start() 重新恢复
    _uploading.clear();
    _resume_pending.clear();
}
