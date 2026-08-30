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
    : _scan_timer(nullptr), _retry_initial_ms(2000), _retry_max_ms(30000),
      _restore_retry_at(0), _connected(false), _db_ready(false),
      _resource_ready(false), _restored(false), _restoring(false), _active_uid(0)
{
    loadRetryConfig();
    //线程边界：公有 API → TCP 线程 slot
    connect(this, &OutboxDispatcher::sig_start, this, &OutboxDispatcher::slot_start);
    //TcpMgr 事件转发（同线程 direct）
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_chat_login_ready,
        this, &OutboxDispatcher::slot_start);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_connection_closed,
        this, &OutboxDispatcher::slot_connection_closed);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_text_msg_rsp_forward,
        this, &OutboxDispatcher::slot_text_msg_rsp);
    connect(TcpMgr::GetInstance().get(), &TcpMgr::sig_resource_msg_meta_rsp_forward,
        this, &OutboxDispatcher::slot_resource_msg_meta_rsp);
    //FileTcpMgr 上传事件（File 线程 → queued 到 TCP 线程）
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_resource_upload_done,
        this, &OutboxDispatcher::slot_resource_upload_done);
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_resource_upload_failed,
        this, &OutboxDispatcher::slot_resource_upload_failed);
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_upload_progress_rsp,
        this, &OutboxDispatcher::slot_upload_progress_rsp);
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_resource_login_success,
        this, &OutboxDispatcher::slot_resource_login_success);
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_resource_login_failed,
        this, &OutboxDispatcher::slot_resource_login_failed);
    connect(FileTcpMgr::GetInstance().get(), &FileTcpMgr::sig_connection_closed,
        this, &OutboxDispatcher::slot_resource_connection_closed);
    //本地库请求/结果信号（worker 线程 → queued 到 TCP 线程）
    auto store = LocalChatStore::GetInstance();
    connect(store.get(), &LocalChatStore::sig_send_enqueued,
        this, &OutboxDispatcher::slot_send_enqueued_registered);
    connect(store.get(), &LocalChatStore::sig_send_confirmed,
        this, &OutboxDispatcher::slot_send_confirmed);
    connect(store.get(), &LocalChatStore::sig_send_failed_marked,
        this, &OutboxDispatcher::slot_send_failed_marked);
    connect(store.get(), &LocalChatStore::sig_resource_confirmed,
        this, &OutboxDispatcher::slot_resource_confirmed);
    connect(store.get(), &LocalChatStore::sig_resource_stage_updated,
        this, &OutboxDispatcher::slot_resource_stage_updated);
    connect(store.get(), &LocalChatStore::sig_outbox_loaded,
        this, &OutboxDispatcher::slot_outbox_loaded);
    connect(store.get(), &LocalChatStore::sig_message_loaded,
        this, &OutboxDispatcher::slot_message_loaded);
    connect(store.get(), &LocalChatStore::sig_db_opened,
        this, &OutboxDispatcher::slot_db_opened);
    connect(store.get(), &LocalChatStore::sig_db_closed,
        this, &OutboxDispatcher::slot_db_closed);
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

void OutboxDispatcher::slot_start()
{
    _connected = true;
    if (!_scan_timer->isActive()) {
        _scan_timer->start();
    }
    if (!_restored) {
        //Chat 登录可能早于 SQLite open 完成；requestRestore 会等待两个条件都成立
        requestRestore();
        return;
    }
    //进程内重连：登记簿仍在——退避中的网络条目立即给一次首发机会，
    //uploading 条目恢复续传（server_message_id 已知则免查库）
    for (auto iter = _entries.begin(); iter != _entries.end(); ++iter) {
        if (iter.value().action == PENDING_NONE) {
            iter.value().next_retry_at = 0;
        }
    }
    resumeUploadingEntries();
    dispatchDueEntries();
}

void OutboxDispatcher::requestRestore()
{
    if (!_connected || !_db_ready || _restored || _restoring) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (_restore_retry_at > now) {
        return;
    }
    _restoring = true;
    LocalChatStore::GetInstance()->loadOutbox();
}

void OutboxDispatcher::slot_db_opened(bool ok, int uid)
{
    if (!ok) {
        //打开失败时不能把内存条目误认为属于一个可用数据库
        _entries.clear();
        _uploading.clear();
        _resume_pending.clear();
        _active_uid = uid;
        _db_ready = false;
        _restored = false;
        _restoring = false;
        _restore_retry_at = 0;
        return;
    }

    if (_db_ready && _active_uid == uid) {
        //同一已打开连接的重复通知不应抹掉正在运行的登记簿
        requestRestore();
        return;
    }

    //登录/切换用户：登记簿对应旧库，整体作废，并立即与当前登录状态会合
    _entries.clear();
    _uploading.clear();
    _resume_pending.clear();
    _active_uid = uid;
    _db_ready = true;
    _restored = false;
    _restoring = false;
    _restore_retry_at = 0;
    requestRestore();
}

void OutboxDispatcher::slot_db_closed()
{
    _entries.clear();
    _uploading.clear();
    _resume_pending.clear();
    _active_uid = 0;
    _db_ready = false;
    _restored = false;
    _restoring = false;
    _restore_retry_at = 0;
}

void OutboxDispatcher::slot_send_enqueued_registered(bool ok, LocalMessageDTO dto,
    OutboxEntryDTO entry)
{
    Q_UNUSED(dto);
    if (!ok) {
        //入库失败：未提交的消息不进登记簿（GUI 已提示）
        return;
    }
    if (_entries.contains(entry.dedup_key)) {
        //恢复窗口内与快照并存/重复投递：以登记簿现值为准
        return;
    }
    RuntimeEntry rt;
    rt.entry = entry;
    rt.next_retry_at = 0;
    _entries.insert(entry.dedup_key, rt);
    //在线即立即首发（queued 投递保序 → 首发顺序=提交顺序）；离线等登录恢复
    dispatchDueEntries();
}

void OutboxDispatcher::slot_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries)
{
    if (!_restoring) {
        //恢复快照以外的全表结果一概不采信（防旧快照复活已销账条目）
        return;
    }
    _restoring = false;
    if (!ok) {
        //库暂不可用：保持未恢复，由扫描定时器按初始退避重试
        _restore_retry_at = QDateTime::currentMSecsSinceEpoch() + _retry_initial_ms;
        qWarning() << "[Outbox] restore loadOutbox failed, retry scheduled";
        return;
    }
    _restore_retry_at = 0;
    _restored = true;
    //insert-if-absent 增量合并：恢复窗口内新提交的条目已在登记簿，不覆盖
    for (const OutboxEntryDTO& entry : entries) {
        if (_entries.contains(entry.dedup_key)) {
            continue;
        }
        RuntimeEntry rt;
        rt.entry = entry;
        rt.next_retry_at = 0;
        _entries.insert(entry.dedup_key, rt);
    }
    //重启恢复：uploading 阶段条目回查 server_message_id 后统一走 1507 对齐续传
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
    //恢复完成：先按落库顺序（operation_id）首发一轮，再放行常规扫描
    dispatchRestoredFirstRound(entries);
    dispatchDueEntries();
}

void OutboxDispatcher::slot_message_loaded(bool ok, LocalMessageDTO dto)
{
    if (!_resume_pending.contains(dto.client_message_id)) {
        return;
    }
    _resume_pending.remove(dto.client_message_id);
    auto iter = _entries.find(dto.client_message_id);
    if (iter == _entries.end()) {
        return;
    }
    if (!ok || dto.server_message_id <= 0) {
        //本地消息行缺失或无 server_message_id，无法续传，按失败收尾
        //（销账落库成功后才离开登记簿）
        qWarning() << "[Outbox] resume upload failed, mark failed:" << dto.client_message_id;
        beginAction(dto.client_message_id, iter.value(), PENDING_MARK_FAILED);
        return;
    }
    iter.value().server_message_id = dto.server_message_id;
    if (_connected && _resource_ready) {
        startResourceUpload(iter.value(), dto.server_message_id);
    }
}

void OutboxDispatcher::dispatchDueEntries()
{
    if (!_connected || !_restored || _restoring) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    //动作结果在测试或未来同线程存储实现中可能同步删除条目，遍历 key 快照避免迭代器失效
    const QList<QString> keys = _entries.keys();
    for (const QString& key : keys) {
        auto iter = _entries.find(key);
        if (iter == _entries.end()) {
            continue;
        }
        RuntimeEntry& rt = iter.value();
        if (rt.next_retry_at > now) {
            continue;
        }
        if (rt.action != PENDING_NONE) {
            //本地事务已发起：到点幂等重发（事务失败/结果丢失的自愈路径）
            scheduleRetry(rt);
            issuePendingAction(rt);
            continue;
        }
        if (rt.entry.operation_type == OUTBOX_OP_SEND_TEXT) {
            dispatchTextEntry(rt);
            continue;
        }
        if (rt.entry.operation_type == OUTBOX_OP_SEND_RESOURCE) {
            dispatchResourceEntry(rt);
            continue;
        }
    }
}

void OutboxDispatcher::dispatchRestoredFirstRound(const QList<OutboxEntryDTO>& order)
{
    //只处理从未首发过的条目（next_retry_at==0），且按落库顺序；恢复窗口内
    //新提交的条目不在 order 里，由随后的 dispatchDueEntries 兜底
    for (const OutboxEntryDTO& entry : order) {
        auto iter = _entries.find(entry.dedup_key);
        if (iter == _entries.end()) {
            continue;
        }
        RuntimeEntry& rt = iter.value();
        if (rt.next_retry_at != 0 || rt.action != PENDING_NONE) {
            continue;
        }
        if (rt.entry.operation_type == OUTBOX_OP_SEND_TEXT) {
            dispatchTextEntry(rt);
        } else if (rt.entry.operation_type == OUTBOX_OP_SEND_RESOURCE) {
            dispatchResourceEntry(rt);
        }
    }
}

void OutboxDispatcher::dispatchTextEntry(RuntimeEntry& rt)
{
    emit TcpMgr::GetInstance()->sig_send_data(ID_TEXT_CHAT_MSG_REQ,
        rt.entry.payload.toUtf8());
    scheduleRetry(rt);
}

void OutboxDispatcher::dispatchResourceEntry(RuntimeEntry& rt)
{
    //uploading 阶段由 1508/1506 事件推进（或重连恢复），扫描不重发 1503
    if (rt.entry.stage != RESOURCE_STAGE_METADATA) {
        return;
    }
    QJsonObject payload = QJsonDocument::fromJson(rt.entry.payload.toUtf8()).object();
    QString local_path = payload["text_or_url"].toString();
    if (!QFile::exists(local_path)) {
        //源文件丢失：标 failed 并停止重传（落库成功才离开登记簿）
        qWarning() << "[Outbox] resource source missing, mark failed:" << local_path;
        beginAction(rt.entry.dedup_key, rt, PENDING_MARK_FAILED);
        return;
    }
    emit TcpMgr::GetInstance()->sig_send_data(ID_CREATE_RESOURCE_MSG_REQ,
        rt.entry.payload.toUtf8());
    scheduleRetry(rt);
}

void OutboxDispatcher::startResourceUpload(RuntimeEntry& rt, qint64 server_message_id)
{
    rt.server_message_id = server_message_id;
    if (!_connected || !_resource_ready) {
        //1507 在 Resource 1502 成功前会被 FileTcpMgr 丢弃，留待 ready 信号统一恢复
        return;
    }
    QJsonObject payload = QJsonDocument::fromJson(rt.entry.payload.toUtf8()).object();
    QString local_path = payload["text_or_url"].toString();
    QString name = payload["file_name"].toString();
    if (!QFile::exists(local_path)) {
        //源文件丢失：标 failed 并停止重传（落库成功才离开登记簿）
        qWarning() << "[Outbox] resource source missing, mark failed:" << local_path;
        beginAction(rt.entry.dedup_key, rt, PENDING_MARK_FAILED);
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
    _uploading.insert(name, rt.entry.request_id);

    //首传/续传统一入口：1507 查服务端真实偏移（.part 长度），1508 回包对齐后窗口续发
    QJsonObject req;
    req["message_id"] = QString::number(server_message_id);
    FileTcpMgr::GetInstance()->SendData(ID_RESOURCE_UPLOAD_PROGRESS_REQ,
        QJsonDocument(req).toJson(QJsonDocument::Compact));
}

void OutboxDispatcher::resumeUploadingEntries()
{
    if (!_connected || !_resource_ready || !_restored || _restoring) {
        return;
    }
    //进程内重连：uploading 条目用运行时 server_message_id 直接 1507 对齐；
    //未知（仅恢复后从未收到 1504/回查结果）才回退查库
    const QList<QString> keys = _entries.keys();
    for (const QString& key : keys) {
        auto iter = _entries.find(key);
        if (iter == _entries.end()) {
            continue;
        }
        RuntimeEntry& rt = iter.value();
        if (rt.entry.operation_type != OUTBOX_OP_SEND_RESOURCE
            || rt.entry.stage != RESOURCE_STAGE_UPLOADING
            || rt.action != PENDING_NONE) {
            continue;
        }
        QJsonObject payload = QJsonDocument::fromJson(rt.entry.payload.toUtf8()).object();
        QString name = payload["file_name"].toString();
        if (_uploading.contains(name) || _resume_pending.contains(rt.entry.request_id)) {
            continue;
        }
        if (rt.server_message_id > 0) {
            startResourceUpload(rt, rt.server_message_id);
        } else {
            _resume_pending.insert(rt.entry.request_id);
            LocalChatStore::GetInstance()->getMessageByClientId(rt.entry.request_id);
        }
    }
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
        //服务端已收齐（重试路径幂等）：按完成收尾——先本地销账，commit 后删登记
        const QString client_message_id = _uploading.value(name);
        _uploading.remove(name);
        auto iter = _entries.find(client_message_id);
        if (iter != _entries.end()) {
            iter.value().server_message_id = message_id;
            beginAction(client_message_id, iter.value(), PENDING_CONFIRM_RESOURCE);
        }
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

void OutboxDispatcher::scheduleRetry(RuntimeEntry& rt)
{
    //指数退避 2s→30s：纯内存运行时状态，重启归零重计，不落盘
    qint64 delay = _retry_initial_ms;
    for (int i = 0; i < rt.retry_count; ++i) {
        delay = qMin(delay * 2, _retry_max_ms);
    }
    rt.retry_count += 1;
    rt.next_retry_at = QDateTime::currentMSecsSinceEpoch() + delay;
}

void OutboxDispatcher::issuePendingAction(RuntimeEntry& rt)
{
    const QString& key = rt.entry.dedup_key;
    switch (rt.action) {
    case PENDING_CONFIRM_TEXT:
        LocalChatStore::GetInstance()->confirmTextSent(key, rt.server_message_id, rt.chat_time);
        break;
    case PENDING_CONFIRM_RESOURCE:
        LocalChatStore::GetInstance()->confirmResourceSent(key);
        break;
    case PENDING_MARK_FAILED:
        LocalChatStore::GetInstance()->markSendFailed(key);
        break;
    case PENDING_ADVANCE_STAGE:
        LocalChatStore::GetInstance()->updateResourceStage(key, rt.server_message_id,
            RESOURCE_STAGE_UPLOADING);
        break;
    default:
        break;
    }
}

void OutboxDispatcher::beginAction(const QString& key, RuntimeEntry& rt, PendingStoreAction action)
{
    Q_UNUSED(key);
    rt.action = action;
    rt.retry_count = 0;
    rt.next_retry_at = 0;
    //先登记下次尝试，再发起事务；即使结果同线程立即返回并删除条目也不留悬空引用
    scheduleRetry(rt);
    issuePendingAction(rt);
}

void OutboxDispatcher::eraseEntry(const QString& key)
{
    _entries.remove(key);
    //连带清理上传会话簿记（key == client_message_id）
    for (auto iter = _uploading.begin(); iter != _uploading.end();) {
        if (iter.value() == key) {
            iter = _uploading.erase(iter);
        } else {
            ++iter;
        }
    }
}

void OutboxDispatcher::failResourceByName(const QString& unique_name)
{
    auto iter = _uploading.find(unique_name);
    if (iter == _uploading.end()) {
        return;
    }
    const QString client_message_id = iter.value();
    _uploading.erase(iter);
    auto ent = _entries.find(client_message_id);
    if (ent == _entries.end()) {
        return;
    }
    beginAction(client_message_id, ent.value(), PENDING_MARK_FAILED);
}

void OutboxDispatcher::slot_scan_timeout()
{
    if (!_connected) {
        _scan_timer->stop();
        return;
    }
    requestRestore();
    dispatchDueEntries();
}

void OutboxDispatcher::slot_text_msg_rsp(int error, QString unique_id, qint64 message_id,
    QString chat_time)
{
    auto iter = _entries.find(unique_id);
    if (iter == _entries.end()) {
        return;
    }
    RuntimeEntry& rt = iter.value();
    if (rt.action != PENDING_NONE) {
        //本地销账已在途，重复/迟到的网络回包不再重复发起事务
        return;
    }
    if (error == ErrorCodes::SUCCESS) {
        //1302 成功：先本地销账事务，commit 成功后才离开登记簿
        rt.server_message_id = message_id;
        rt.chat_time = chat_time;
        beginAction(unique_id, rt, PENDING_CONFIRM_TEXT);
        return;
    }
    if (error == ErrorCodes::MESSAGE_CONFLICT) {
        //永久冲突：标 SEND_FAILED 停止重传（落库成功才删登记）
        beginAction(unique_id, rt, PENDING_MARK_FAILED);
        return;
    }
    //transient（2014/2016）：本次网络发送时已经安排了下一次退避，不重复翻倍
}

void OutboxDispatcher::slot_resource_msg_meta_rsp(int error, QString unique_id, QString file_name,
    qint64 message_id, qint64 thread_id, qint64 fromuid, qint64 touid)
{
    Q_UNUSED(file_name);
    Q_UNUSED(thread_id);
    Q_UNUSED(fromuid);
    Q_UNUSED(touid);
    auto iter = _entries.find(unique_id);
    if (iter == _entries.end()) {
        return;
    }
    RuntimeEntry& rt = iter.value();
    if (rt.action != PENDING_NONE) {
        return;
    }
    //permanent：冲突/资源元数据非法/超限，标 SEND_FAILED 停止重传
    if (error == ErrorCodes::MESSAGE_CONFLICT
        || error == ErrorCodes::RESOURCE_INVALID
        || error == ErrorCodes::RESOURCE_SIZE_EXCEEDED) {
        beginAction(unique_id, rt, PENDING_MARK_FAILED);
        return;
    }
    if (error != ErrorCodes::SUCCESS) {
        //transient（2014/2016）：沿用发送时已经安排的退避期限
        return;
    }
    //1504 成功：先落库 stage=uploading（不删条目），commit 成功后才推进内存
    //并启动上传（失败重试只重发 stage 推进，不重发 1503）
    rt.server_message_id = message_id;
    beginAction(unique_id, rt, PENDING_ADVANCE_STAGE);
}

void OutboxDispatcher::slot_send_confirmed(bool ok, LocalMessageDTO dto)
{
    auto iter = _entries.find(dto.client_message_id);
    if (iter == _entries.end() || iter.value().action != PENDING_CONFIRM_TEXT) {
        return;
    }
    if (ok) {
        //销账事务已提交：登记簿才允许删除
        eraseEntry(dto.client_message_id);
        return;
    }
    //本地销账失败：发送动作时已安排退避，登记保留等待扫描重试
}

void OutboxDispatcher::slot_send_failed_marked(bool ok, LocalMessageDTO dto)
{
    auto iter = _entries.find(dto.client_message_id);
    if (iter == _entries.end() || iter.value().action != PENDING_MARK_FAILED) {
        return;
    }
    if (ok) {
        eraseEntry(dto.client_message_id);
        return;
    }
    //动作发起时已安排下一次重试
}

void OutboxDispatcher::slot_resource_confirmed(bool ok, LocalMessageDTO dto)
{
    auto iter = _entries.find(dto.client_message_id);
    if (iter == _entries.end() || iter.value().action != PENDING_CONFIRM_RESOURCE) {
        return;
    }
    if (ok) {
        eraseEntry(dto.client_message_id);
        return;
    }
    //动作发起时已安排下一次重试
}

void OutboxDispatcher::slot_resource_stage_updated(bool ok, LocalMessageDTO dto)
{
    auto iter = _entries.find(dto.client_message_id);
    if (iter == _entries.end() || iter.value().action != PENDING_ADVANCE_STAGE) {
        return;
    }
    if (!ok) {
        //stage 推进落库失败：动作发起时已安排退避，到点幂等重试
        return;
    }
    //磁盘已到 uploading：内存才允许推进并启动 1507 上传
    RuntimeEntry& rt = iter.value();
    rt.action = PENDING_NONE;
    rt.entry.stage = RESOURCE_STAGE_UPLOADING;
    rt.server_message_id = dto.server_message_id;
    rt.retry_count = 0;
    rt.next_retry_at = 0;
    if (_connected && _resource_ready) {
        startResourceUpload(rt, dto.server_message_id);
    }
}

void OutboxDispatcher::slot_resource_login_success()
{
    _resource_ready = true;
    resumeUploadingEntries();
}

void OutboxDispatcher::slot_resource_login_failed(QString reason)
{
    Q_UNUSED(reason);
    slot_resource_connection_closed();
}

void OutboxDispatcher::slot_resource_connection_closed()
{
    _resource_ready = false;
    _uploading.clear();
    _resume_pending.clear();
}

void OutboxDispatcher::slot_resource_upload_done(QString unique_name)
{
    auto iter = _uploading.find(unique_name);
    if (iter == _uploading.end()) {
        return;
    }
    QString client_message_id = iter.value();
    _uploading.erase(iter);
    auto ent = _entries.find(client_message_id);
    if (ent == _entries.end()) {
        return;
    }
    //1506 resource_status=Ready：先本地销账（sent + 删 outbox），commit 后删登记
    beginAction(client_message_id, ent.value(), PENDING_CONFIRM_RESOURCE);
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
        //2111 消息不存在（DB 行缺失）等：按失败收尾；transient 错误交由重连恢复兜底
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

void OutboxDispatcher::slot_connection_closed()
{
    _connected = false;
    _scan_timer->stop();
    //断线后上传会话失效（重连恢复）；登记簿保留，重连按内存继续
    _uploading.clear();
    _resume_pending.clear();
}
