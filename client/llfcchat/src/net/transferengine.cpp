#include "transferengine.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "protocol_ids.h"
#include "resourcecodec.h"

#include <deque>

//chat_messages.status（服务端 1506/1508/1510 回包的 status 字段）。
namespace {
const int kStatusPending = 0;
const int kStatusPublished = 1;
const int kStatusFailed = 2;

//永久失败：不再重试，直接终态。
bool IsPermanentError(int error)
{
    switch (error) {
    case llfc_proto::RS_MSG_ID_ERR:              //2111 消息不存在
    case llfc_proto::RS_FILE_SIZE_EXCEEDED:      //2113 超类型上限
    case llfc_proto::RS_RESOURCE_FORBIDDEN:      //2115 非成员
    case llfc_proto::RS_RESOURCE_STATE_INVALID:  //2116 已失败/就绪后非法操作
        return true;
    default:
        return false;
    }
}

//回退类错误：按回包 server_offset 对齐重发（2112 整文件时服务端已删 .part，
//server_offset=0 即整文件重传）。
bool IsRollbackError(int error)
{
    return error == llfc_proto::RS_FILE_OFFSET_INVALID   //2107 偏移超前
        || error == llfc_proto::RS_FILE_HASH_MISMATCH;   //2112 分片哈希不符
}

//暂时错误：退避 1s→30s 后重新 1507/1509 同步。
bool IsRetryableError(int error)
{
    switch (error) {
    case llfc_proto::ERR_RPC_FAILED:               //2002
    case llfc_proto::RS_CREATE_FILE_PATH_FAILED:   //2103
    case llfc_proto::RS_FILE_WRITE_PERMISSION:     //2104
    case llfc_proto::RS_FILE_READ_FAILED:          //2108
    case llfc_proto::RS_RESOURCE_NOT_READY:        //2114
        return true;
    default:
        return false;
    }
}

QString Sha256HexOf(const QByteArray& raw)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
}

QString PartPathOf(const QString& dest)
{
    return dest + QString::fromLatin1(".part");
}

} // namespace

//在途分片登记（上传含发片哈希；下载 length 为请求时的预期长度，仅作窗口
//字节记账）。sent_ms 用于队首超时判定。
struct InflightChunk {
    qint64 offset;
    qint64 length;
    QString sha256;
    qint64 sent_ms;
};

//引擎运行时状态：QFile/QCryptographicHash 为受控裸指针（CloseFiles 统一
//收口），整个对象只经 shared_ptr 存于方向分立的登记表，绝不复制。
struct ResourceTransferRuntime {
    qint64 message_id;
    int direction; //TransferDirection
    TransferPhase phase;
    bool slot_granted; //是否占用 2上传/4下载 的活跃槽位

    qint64 total_bytes;
    qint64 acked_bytes; //上传=服务端确认；下载=已落盘
    qint64 wire_bytes;  //发送窗口前缘（已交付传输层的下一偏移）
    qint64 chunk_size;  //1508 可按服务端回包校正

    int error_code;
    QString error_text;
    qint64 event_seq;

    int retry_count;         //退避计数（指数）
    int json_error_count;    //连续 2001/坏回包
    int chunk_retry_count;   //单分片连续失败（下载校验失败/2112）
    int publish_retry_count; //收齐后重发末片计数（防完成点热循环）
    qint64 retry_at_ms;      //退避唤醒时刻
    qint64 query_sent_ms;    //1507/1509 在途时刻（0=未发）
    bool reconnect_requested; //本传输已触发过重连请求（鉴权成功后清除）
    bool user_paused;         //用户暂停意图：独立于 phase，覆盖 Queued/
                              //Querying/Backoff/Active；断开重连不解除，
                              //仅在 ResumeTransfer 清除。暂停期间一切发片
                              //与 1507/1509 控制补发都被抑制，迟到回包仍
                              //可保存元数据或按 Published 完成。
    //P2 秒传：1514 回 2118（证明不符/挑战丢失/TTL 过期）后置真——本传输
    //内忽略后续 mode=proof 挑战，直接普通上传（客户端防无限挑战闸门，
    //与服务端 challenge_suppressed 双端闭环）。重连/暂停不重置；
    //Reset/终态重启（新运行时登记）自然清零，保持用户会话隔离。
    bool proof_disabled;
    //P2 秒传：1513 已发出、正在等 1514（匹配的控制帧）。置真期间到达的 1508
    //一律视为上一轮 1507 的迟到回包而丢弃，1514 也只在置真时才被采纳——
    //否则一条陈旧应答会清掉当前控制定时器或回退正在进行的普通上传窗口。
    //新控制帧（BeginQuery/1507、超时、暂停、断连）一律清零。
    bool proof_pending;

    std::deque<InflightChunk> inflight; //FIFO 在途窗口
    std::deque<qint64> stale_offsets;   //回退后等待排空的迟到回包（升序）

    //上传
    UploadRequest upload;
    QFile* source;
    //上传源文件延迟打开：Start 只保存请求（排队任务不占 FD），获得槽位
    //且 1508 确认 Pending 后 EnsureSourceOpen 才检查/打开；已 Published 的
    //消息无需源文件，直接完成（不得因源被删误标 Failed）。

    //下载
    DownloadRequest download;
    QFile* part;                  //持久打开的 "<dest>.part"
    QString expected_sha256;      //1510 为准（可回落到请求值）
    QCryptographicHash* hasher;   //增量哈希
    qint64 hashed_len;            //已增量入哈希的字节数
    bool hash_valid;              //false→完成时回退整文件哈希
    QString local_download_path;  //下载完成后的最终路径（快照携带）
};

TransferEngine::TransferEngine(QObject* parent)
    : QObject(parent), _tick(new QTimer(this)), _conn_up(false),
      _awaiting_reconnect(false)
{
    TransferEngine::Config defaults;
    InitConfig(defaults);
}

TransferEngine::TransferEngine(const Config& config, QObject* parent)
    : QObject(parent), _tick(new QTimer(this)), _conn_up(false),
      _awaiting_reconnect(false)
{
    InitConfig(config);
}

void TransferEngine::InitConfig(const Config& config)
{
    _cfg = config;
    //配置下限：防 0/负值把 tick、退避、窗口退化为热循环或死等。
    if (_cfg.max_active_uploads < 1) _cfg.max_active_uploads = 1;
    if (_cfg.max_active_downloads < 1) _cfg.max_active_downloads = 1;
    if (_cfg.max_inflight_chunks < 1) _cfg.max_inflight_chunks = 1;
    if (_cfg.ack_timeout_ms < 100) _cfg.ack_timeout_ms = 100;
    if (_cfg.backoff_initial_ms < 100) _cfg.backoff_initial_ms = 100;
    if (_cfg.backoff_max_ms < _cfg.backoff_initial_ms) {
        _cfg.backoff_max_ms = _cfg.backoff_initial_ms;
    }
    if (_cfg.json_error_limit < 1) _cfg.json_error_limit = 1;
    if (_cfg.chunk_retry_limit < 1) _cfg.chunk_retry_limit = 1;
    //wire 分片单位固定为共享常量：不接受任意传入值（防旧 32KiB 配置/1B 退化）
    _cfg.chunk_size = static_cast<qint64>(llfc_wire::kResourceChunkSize);
    if (_cfg.tick_interval_ms < 10) _cfg.tick_interval_ms = 10;

    //快照走跨线程 queued 信号，先注册 metatype（幂等）。
    qRegisterMetaType<TransferSnapshot>("TransferSnapshot");
    qRegisterMetaType<UploadRequest>("UploadRequest");
    qRegisterMetaType<DownloadRequest>("DownloadRequest");

    connect(_tick, &QTimer::timeout, this, &TransferEngine::OnTick);
    _clock.start();
    _tick->start(_cfg.tick_interval_ms);
}

TransferEngine::~TransferEngine()
{
    CloseAll();
}

void TransferEngine::CloseAll()
{
    for (RuntimeMap::iterator it = _uploads.begin(); it != _uploads.end(); ++it) {
        CloseFiles(it.value().get());
    }
    for (RuntimeMap::iterator it = _downloads.begin(); it != _downloads.end();
         ++it) {
        CloseFiles(it.value().get());
    }
}

void TransferEngine::CloseFiles(ResourceTransferRuntime* t)
{
    delete t->source;
    t->source = 0;
    delete t->part;
    t->part = 0;
    delete t->hasher;
    t->hasher = 0;
}

qint64 TransferEngine::NowMs() const
{
    return _clock.elapsed();
}

void TransferEngine::SetSendCallback(SendFn send)
{
    _send = send;
}

TransferEngine::RuntimeMap& TransferEngine::MapFor(int direction)
{
    return direction == TRANSFER_UPLOAD ? _uploads : _downloads;
}

const TransferEngine::RuntimeMap& TransferEngine::MapFor(int direction) const
{
    return direction == TRANSFER_UPLOAD ? _uploads : _downloads;
}

ResourceTransferRuntime* TransferEngine::FindActive(int direction,
    qint64 message_id)
{
    RuntimeMap& m = MapFor(direction);
    RuntimeMap::iterator it = m.find(message_id);
    if (it == m.end()) {
        return 0;
    }
    ResourceTransferRuntime* t = it.value().get();
    if (t->phase == TRANSFER_COMPLETED || t->phase == TRANSFER_FAILED) {
        return 0;
    }
    return t;
}

int TransferEngine::ActiveCount(int direction) const
{
    int n = 0;
    const RuntimeMap& m = MapFor(direction);
    for (RuntimeMap::const_iterator it = m.begin(); it != m.end(); ++it) {
        const ResourceTransferRuntime* t = it.value().get();
        if (t->slot_granted && t->phase != TRANSFER_COMPLETED
            && t->phase != TRANSFER_FAILED) {
            ++n;
        }
    }
    return n;
}

void TransferEngine::PromoteWaiting(int direction)
{
    const int limit = direction == TRANSFER_UPLOAD
        ? _cfg.max_active_uploads : _cfg.max_active_downloads;
    QQueue<qint64>& wait = direction == TRANSFER_UPLOAD
        ? _upload_wait : _download_wait;
    RuntimeMap& m = MapFor(direction);
    //按索引扫描等待队列（B1 修正）：清理无效项（登记已不存在/终态/方向
    //不符/已持有槽位），跳过暂停项（保留其相对顺序与恢复资格），把最早的
    //可调度项出队授槽。旧实现队首暂停即 break，A/B 活跃、C 排队暂停、
    //D 排队未暂停时，A 完成释放的空闲槽永远轮不到 D。
    while (ActiveCount(direction) < limit && !wait.isEmpty()) {
        int pick = -1;
        for (int i = 0; i < wait.size(); ++i) {
            const qint64 id = wait.at(i);
            RuntimeMap::iterator it = m.find(id);
            if (it == m.end()) {
                wait.removeAt(i); //已被重启替换，不再是等待者
                --i;
                continue;
            }
            ResourceTransferRuntime* t = it.value().get();
            if (t->phase == TRANSFER_COMPLETED || t->phase == TRANSFER_FAILED
                || t->direction != direction || t->slot_granted) {
                wait.removeAt(i); //终态/已授槽：不再是等待者
                --i;
                continue;
            }
            if (t->user_paused) {
                continue; //暂停者保留队位与相对顺序，仅本轮不可调度
            }
            pick = i;
            break;
        }
        if (pick < 0) {
            break; //无可调度项（全部暂停或队列已清空）
        }
        RuntimeMap::iterator it = m.find(wait.at(pick));
        ResourceTransferRuntime* t = it.value().get();
        wait.removeAt(pick);
        t->slot_granted = true;
        BeginQuery(t);
        EmitUpdate(t);
    }
}

//按 Start 参数初始化一张新的运行时登记（方向、key 均已由调用方校验）。
static void InitRuntime(ResourceTransferRuntime* t, int direction,
    qint64 message_id, qint64 chunk_size)
{
    t->message_id = message_id;
    t->direction = direction;
    t->phase = TRANSFER_QUEUED;
    t->slot_granted = false;
    t->total_bytes = 0;
    t->acked_bytes = 0;
    t->wire_bytes = 0;
    t->chunk_size = chunk_size;
    t->error_code = 0;
    t->event_seq = 0;
    t->retry_count = 0;
    t->json_error_count = 0;
    t->chunk_retry_count = 0;
    t->publish_retry_count = 0;
    t->retry_at_ms = 0;
    t->query_sent_ms = 0;
    t->reconnect_requested = false;
    t->user_paused = false;
    t->proof_disabled = false;
    t->proof_pending = false;
    t->source = 0;
    t->part = 0;
    t->hasher = 0;
    t->hashed_len = 0;
    t->hash_valid = true;
}

bool TransferEngine::StartUpload(const UploadRequest& req)
{
    if (req.message_id <= 0 || req.file_path.isEmpty()) {
        return false;
    }
    RuntimeMap& m = MapFor(TRANSFER_UPLOAD);
    RuntimeMap::iterator old = m.find(req.message_id);
    if (old != m.end()) {
        //非终态（活跃/排队/退避/暂停）→重复启动直接忽略；终态记录允许重启。
        if (old.value()->phase != TRANSFER_COMPLETED
            && old.value()->phase != TRANSFER_FAILED) {
            return true;
        }
        m.erase(old);
    }
    std::shared_ptr<ResourceTransferRuntime> t(new ResourceTransferRuntime());
    InitRuntime(t.get(), TRANSFER_UPLOAD, req.message_id, _cfg.chunk_size);
    t->upload = req;
    t->total_bytes = req.file_size_bytes;

    //空文件协议不可表达（服务端 0 长分片直接拒绝），本地参数即终态。
    if (req.file_size_bytes <= 0) {
        FailTransfer(t.get(), 0, QString::fromLatin1("empty source file"));
        m.insert(req.message_id, t);
        return false;
    }
    //延迟打开源文件：Start 只保存请求——1000 个排队任务不占 1000 个 FD；
    //获得槽位且 1508 确认 Pending 后 EnsureSourceOpen 才检查存在/大小并
    //打开；已 Published 无需源文件直接完成（不得因源被删误标 Failed）。
    m.insert(req.message_id, t);
    if (ActiveCount(TRANSFER_UPLOAD) >= _cfg.max_active_uploads) {
        _upload_wait.enqueue(req.message_id); //排队等槽位，快照保持 Queued
        EmitUpdate(t.get());
        return true;
    }
    t->slot_granted = true;
    BeginQuery(t.get());
    EmitUpdate(t.get());
    return true;
}

bool TransferEngine::StartDownload(const DownloadRequest& req)
{
    if (req.message_id <= 0 || req.dest_file_path.isEmpty()) {
        return false;
    }
    RuntimeMap& m = MapFor(TRANSFER_DOWNLOAD);
    RuntimeMap::iterator old = m.find(req.message_id);
    if (old != m.end()) {
        if (old.value()->phase != TRANSFER_COMPLETED
            && old.value()->phase != TRANSFER_FAILED) {
            return true; //活跃中：幂等忽略
        }
        m.erase(old); //终态允许重新下载（本地缓存已删等场景）
    }
    std::shared_ptr<ResourceTransferRuntime> t(new ResourceTransferRuntime());
    InitRuntime(t.get(), TRANSFER_DOWNLOAD, req.message_id, _cfg.chunk_size);
    t->download = req;
    t->total_bytes = req.file_size_bytes; //0=未知，1510 校正
    t->expected_sha256 = req.sha256;

    //目标目录先行就绪，完成后 rename 不因目录缺失失败。
    const QFileInfo fi(req.dest_file_path);
    if (!fi.dir().exists() && !QDir().mkpath(fi.absolutePath())) {
        FailTransfer(t.get(), 0, QString::fromLatin1("dest dir create failed"));
        m.insert(req.message_id, t);
        return false;
    }
    m.insert(req.message_id, t);
    if (ActiveCount(TRANSFER_DOWNLOAD) >= _cfg.max_active_downloads) {
        _download_wait.enqueue(req.message_id);
        EmitUpdate(t.get());
        return true;
    }
    t->slot_granted = true;
    BeginQuery(t.get());
    EmitUpdate(t.get());
    return true;
}

void TransferEngine::PauseTransfer(int direction, qint64 message_id)
{
    //暂停=独立意图标志，覆盖 Queued/Querying/Backoff/Active 任意阶段：
    //不迁移 phase（内部状态机照常排空在途回包），只抑制一切发片与
    //1507/1509 控制补发；已授予的槽位保留；排队中的传输保持队首资格。
    ResourceTransferRuntime* t = FindActive(direction, message_id);
    if (!t || t->user_paused) {
        return;
    }
    t->user_paused = true;
    //P2：暂停只抑制"新请求"（不发片、不补发 1507/1509）
    //——**不动在途控制帧状态**（query_sent_ms/proof_pending 保留）：
    //已在途的 1513 如果服务端已发布，那条严格校验通过的 Published 1514
    //仍能收敛本传输（暂停不该把"确实已发布"的内容丢弃，否则只能等 Resume
    //重查）。隔离旧阶段由 Resume/Resync/重连统一完成（BeginQuery 清标记），
    //BeginQuery 在 user_paused 下不发帧，故暂停期不会产生新请求。
    EmitUpdate(t); //快照投影为 PAUSED
}

void TransferEngine::ResumeTransfer(int direction, qint64 message_id)
{
    RuntimeMap& m = MapFor(direction);
    RuntimeMap::iterator it = m.find(message_id);
    if (it == m.end()) {
        return;
    }
    ResourceTransferRuntime* t = it.value().get();
    if (t->phase == TRANSFER_COMPLETED || t->phase == TRANSFER_FAILED
        || !t->user_paused) {
        return;
    }
    t->user_paused = false;
    if (!t->slot_granted) {
        //仍在等待队列：保持原队位（不越槽），槽位空出时 PromoteWaiting
        //按 FIFO 授予；这里只尝试推进一次。
        PromoteWaiting(direction);
        EmitUpdate(t);
        return;
    }
    if (t->direction == TRANSFER_UPLOAD) {
        //上传恢复统一重发 1507（FileBubble"继续"即此路径）。
        ResyncUpload(t);
    } else {
        //下载恢复：1509 重查并以 .part 真值重建窗口（鉴权/状态可能已变）。
        BeginQuery(t);
    }
    EmitUpdate(t);
}

void TransferEngine::CancelTransfer(int direction, qint64 message_id)
{
    ResourceTransferRuntime* t = FindActive(direction, message_id);
    if (!t) {
        return;
    }
    //保留 .part 与源文件不动；仅本地终态化。
    FailTransfer(t, 0, QString::fromLatin1("cancelled"));
}

bool TransferEngine::GetSnapshot(int direction, qint64 message_id,
    TransferSnapshot* out) const
{
    const RuntimeMap& m = MapFor(direction);
    RuntimeMap::const_iterator it = m.find(message_id);
    if (it == m.end() || !out) {
        return false;
    }
    *out = MakeSnapshot(it.value().get());
    return true;
}

void TransferEngine::OnConnectionDown()
{
    _conn_up = false;
    _awaiting_reconnect = false;
    for (int dir = 0; dir <= 1; ++dir) {
        RuntimeMap& m = MapFor(dir);
        for (RuntimeMap::iterator it = m.begin(); it != m.end(); ++it) {
            ResourceTransferRuntime* t = it.value().get();
            if (t->phase == TRANSFER_COMPLETED || t->phase == TRANSFER_FAILED) {
                continue;
            }
            //连接已死，在途回包不会到达：直接清空（不留过期队列）。
            t->inflight.clear();
            t->stale_offsets.clear();
            t->wire_bytes = t->acked_bytes;
            t->query_sent_ms = 0;
            t->proof_pending = false; //旧连接的在途 1513/1507 全部作废
            t->reconnect_requested = false;
        }
    }
}

void TransferEngine::Reset()
{
    //静默重置（用户代次切换/主动登出）：A 的传输不得在 B 登录后被重启。
    //关闭全部文件句柄、清空两方向登记表与等待队列；不发终态/更新信号。
    CloseAll();
    _uploads.clear();
    _downloads.clear();
    _upload_wait.clear();
    _download_wait.clear();
    _conn_up = false;
    _awaiting_reconnect = false;
}

void TransferEngine::OnConnectionAuthenticated()
{
    _conn_up = true;
    _awaiting_reconnect = false;
    for (int dir = 0; dir <= 1; ++dir) {
        RuntimeMap& m = MapFor(dir);
        for (RuntimeMap::iterator it = m.begin(); it != m.end(); ++it) {
            ResourceTransferRuntime* t = it.value().get();
            if (!t->slot_granted || t->phase == TRANSFER_COMPLETED
                || t->phase == TRANSFER_FAILED || t->user_paused) {
                continue; //断开重连不解除用户暂停
            }
            t->reconnect_requested = false;
            t->retry_count = 0; //重连是全新机会，退避从头计
            BeginQuery(t);      //上传重发 1507；下载重发 1509（鉴权缓存已失效）
        }
    }
    Pump();
}

void TransferEngine::OnFrame(quint16 msg_id, const QByteArray& payload)
{
    switch (msg_id) {
    case llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_RSP: //1506
        HandleChunkUploadRsp(payload);
        break;
    case llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_RSP: //1508
        HandleProgressRsp(payload);
        break;
    case llfc_proto::MSG_RESOURCE_DOWN_INFO_RSP: //1510
        HandleDownInfoRsp(payload);
        break;
    case llfc_proto::MSG_RESOURCE_CHUNK_DOWN_RSP: //1512
        HandleChunkDownRsp(payload);
        break;
    case llfc_proto::MSG_RESOURCE_INSTANT_PROOF_RSP: //1514（P2 秒传）
        HandleInstantProofRsp(payload);
        break;
    default:
        break; //非引擎协议（1502 等由传输层处理）
    }
}

void TransferEngine::BeginQuery(ResourceTransferRuntime* t)
{
    //新控制帧（1507/1509）取代任何在途 1513：迟到的 1514 不再匹配本阶段
    //（重连/重同步/暂停恢复都经此处，保证挑战状态与用户会话隔离）。
    t->proof_pending = false;
    if (!_conn_up || t->user_paused) {
        //连接未就绪或用户暂停：不发控制帧，等鉴权成功/Resume 统一发起。
        t->phase = TRANSFER_QUERYING;
        t->query_sent_ms = 0;
        return;
    }
    t->phase = TRANSFER_QUERYING;
    t->query_sent_ms = NowMs();
    if (t->direction == TRANSFER_UPLOAD) {
        if (!_send) {
            qWarning() << "[TransferEngine] no send callback, 1507 dropped msg="
                       << t->message_id;
            return;
        }
        //旧在途转过期队列：1508 回来后按服务端真值重建窗口。
        while (!t->inflight.empty()) {
            t->stale_offsets.push_back(t->inflight.front().offset);
            t->inflight.pop_front();
        }
        _send(llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_REQ,
            rc::EncodeProgressReq(t->message_id));
    } else {
        //下载同步即 1509；.part 真值在 1510 回来后重建，旧状态全部作废。
        t->inflight.clear();
        t->stale_offsets.clear();
        if (!_send) {
            qWarning() << "[TransferEngine] no send callback, 1509 dropped msg="
                       << t->message_id;
            return;
        }
        _send(llfc_proto::MSG_RESOURCE_DOWN_INFO_REQ,
            rc::EncodeDownInfoReq(t->message_id));
    }
}

void TransferEngine::RollbackUploadTo(ResourceTransferRuntime* t,
    qint64 server_offset)
{
    while (!t->inflight.empty()) {
        t->stale_offsets.push_back(t->inflight.front().offset);
        t->inflight.pop_front();
    }
    qint64 so = server_offset >= 0 ? server_offset : 0;
    if (so > t->total_bytes) {
        so = t->total_bytes;
    }
    if (t->chunk_size > 0) {
        so = (so / t->chunk_size) * t->chunk_size; //防御性对齐分片边界
    }
    //服务端真值即当前确认位（2112 整文件时 server 已删 .part，so=0）。
    t->acked_bytes = so;
    t->wire_bytes = so;
}

void TransferEngine::ResyncUpload(ResourceTransferRuntime* t)
{
    RollbackUploadTo(t, t->acked_bytes);
    BeginQuery(t);
}

void TransferEngine::ScheduleBackoff(ResourceTransferRuntime* t)
{
    t->phase = TRANSFER_BACKOFF;
    t->retry_at_ms = NowMs() + BackoffIntervalMs(t->retry_count);
    t->retry_count += 1;
}

qint64 TransferEngine::BackoffIntervalMs(int retry_count) const
{
    const int shift = retry_count < 5 ? retry_count : 5;
    qint64 v = _cfg.backoff_initial_ms << shift;
    if (v <= 0 || v > _cfg.backoff_max_ms) {
        v = _cfg.backoff_max_ms; //1s→2s→4s→8s→16s→30s 封顶
    }
    return v;
}

void TransferEngine::OnTick()
{
    const qint64 now = NowMs();
    for (int dir = 0; dir <= 1; ++dir) {
        //快照 key 再迭代：HandleTimeout 会发信号，直接连接的槽可能同步
        //Start* 插入登记表，QHash 迭代子会失效。
        const QList<qint64> ids = MapFor(dir).keys();
        for (int i = 0; i < ids.size(); ++i) {
            ResourceTransferRuntime* t = FindActive(dir, ids.at(i));
            if (!t) {
                continue;
            }
            //退避到期：连接就绪则重新同步；未就绪稍后再查。
            if (t->phase == TRANSFER_BACKOFF && t->retry_at_ms > 0
                && now >= t->retry_at_ms) {
                t->retry_at_ms = 0;
                if (_conn_up) {
                    BeginQuery(t);
                } else {
                    t->retry_at_ms = now + _cfg.backoff_initial_ms;
                }
                continue;
            }
            //队首在途 30s 无回包：连接死亡判定。
            if (t->phase == TRANSFER_ACTIVE && !t->inflight.empty()
                && now - t->inflight.front().sent_ms >= _cfg.ack_timeout_ms) {
                HandleTimeout(t);
                continue;
            }
            //1507/1509 控制请求同样受超时约束。
            if (t->phase == TRANSFER_QUERYING && t->query_sent_ms > 0
                && now - t->query_sent_ms >= _cfg.ack_timeout_ms) {
                HandleTimeout(t);
            }
        }
    }
}

void TransferEngine::HandleTimeout(ResourceTransferRuntime* t)
{
    //判定连接已死：清在途（回包视为不可达），窗口回退到确认位。
    t->inflight.clear();
    t->stale_offsets.clear();
    t->wire_bytes = t->acked_bytes;
    t->query_sent_ms = 0;
    if (t->user_paused) {
        //暂停期不判连接死亡、不请求重连（用户意图优先；恢复统一经
        //ResumeTransfer → 1507 重同步）。在途 proof 标记保留：严格校验
        //通过的 Published 1514 仍可在恢复前收敛本传输；已超时的控制帧
        //自然作废（query_sent_ms=0 后不再触发超时）。
        return;
    }
    t->proof_pending = false; //在途 1513 随连接死亡作废（迟到 1514 不再匹配）
    t->reconnect_requested = true;
    if (!_awaiting_reconnect) {
        _awaiting_reconnect = true;
        emit sig_request_reconnect();
    }
}

void TransferEngine::Pump()
{
    if (!_conn_up) {
        return;
    }
    //按字节窗口公平调度：每轮给"在途字节最少"的活跃传输补一片。
    //扫描轮内不发帧，发送（可能重入插表）后从头重扫，迭代子安全。
    for (;;) {
        ResourceTransferRuntime* best = 0;
        qint64 best_bytes = 0;
        for (int dir = 0; dir <= 1; ++dir) {
            RuntimeMap& m = MapFor(dir);
            for (RuntimeMap::iterator it = m.begin(); it != m.end(); ++it) {
                ResourceTransferRuntime* t = it.value().get();
                if (t->phase != TRANSFER_ACTIVE || !t->slot_granted
                    || t->reconnect_requested || t->user_paused
                    || t->inflight.size()
                        >= static_cast<size_t>(_cfg.max_inflight_chunks)
                    || t->wire_bytes >= t->total_bytes) {
                    continue;
                }
                qint64 bytes = 0;
                for (size_t i = 0; i < t->inflight.size(); ++i) {
                    bytes += t->inflight[i].length;
                }
                if (best == 0 || bytes < best_bytes) {
                    best = t;
                    best_bytes = bytes;
                }
            }
        }
        if (best == 0) {
            break;
        }
        if (!SendNextChunk(best)) {
            break; //发片失败（本地 IO），避免热循环
        }
    }
}

bool TransferEngine::SendNextChunk(ResourceTransferRuntime* t)
{
    if (t->direction == TRANSFER_UPLOAD) {
        const qint64 offset = t->wire_bytes;
        const qint64 len = qMin<qint64>(t->chunk_size, t->total_bytes - offset);
        if (len <= 0 || t->inflight.size()
            >= static_cast<size_t>(_cfg.max_inflight_chunks)) {
            return false;
        }
        if (!t->source || !t->source->seek(offset)) {
            FailTransfer(t, 0, QString::fromLatin1("local source seek failed"));
            return false;
        }
        const QByteArray raw = t->source->read(len);
        if (raw.size() != len) {
            FailTransfer(t, 0, QString::fromLatin1("local source read failed"));
            return false;
        }
        //分片哈希发片时现算（删除预计算 _chunk_hashes 表）。
        InflightChunk c;
        c.offset = offset;
        c.length = len;
        c.sha256 = Sha256HexOf(raw);
        c.sent_ms = NowMs();
        t->inflight.push_back(c);
        t->wire_bytes = offset + len;
        if (!_send) {
            qWarning() << "[TransferEngine] no send callback, 1505 dropped msg="
                       << t->message_id;
            return false;
        }
        _send(llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_REQ,
            rc::EncodeChunkUploadReq(t->message_id, offset, c.sha256, raw));
        return true;
    }
    //下载：按已确认位对齐请求下一片。
    const qint64 offset = t->wire_bytes;
    const qint64 len = qMin<qint64>(t->chunk_size, t->total_bytes - offset);
    if (len <= 0 || t->inflight.size()
        >= static_cast<size_t>(_cfg.max_inflight_chunks)) {
        return false;
    }
    InflightChunk c;
    c.offset = offset;
    c.length = len; //预期长度，仅窗口记账
    c.sent_ms = NowMs();
    t->inflight.push_back(c);
    t->wire_bytes = offset + len;
    if (!_send) {
        qWarning() << "[TransferEngine] no send callback, 1511 dropped msg="
                   << t->message_id;
        return false;
    }
    _send(llfc_proto::MSG_RESOURCE_CHUNK_DOWN_REQ,
        rc::EncodeChunkDownReq(t->message_id, offset));
    return true;
}

void TransferEngine::HandleUploadError(ResourceTransferRuntime* t, int error,
    qint64 server_offset)
{
    if (IsPermanentError(error)) {
        FailTransfer(t, error, QString::fromLatin1("permanent server error"));
        return;
    }
    if (error == llfc_proto::RS_FILE_CONTENT_MISMATCH) { //2117
        VerifySourceHashOn2117(t, server_offset);
        return;
    }
    //非回退类错误若回包缺 server_offset，回退到已确认位即可（无需归零）。
    const qint64 fallback = server_offset >= 0 ? server_offset : t->acked_bytes;
    if (IsRollbackError(error)) { //2107/2112
        RollbackUploadTo(t, server_offset);
        t->chunk_retry_count += 1;
        if (t->chunk_retry_count >= _cfg.chunk_retry_limit) {
            t->chunk_retry_count = 0;
            ScheduleBackoff(t); //连续回退转退避，退避后 1507 重同步
        } else {
            t->phase = TRANSFER_ACTIVE;
            Pump();
        }
        EmitUpdate(t);
        return;
    }
    //2001/坏回包：连续 json_error_limit 次转永久。
    if (error == llfc_proto::ERR_JSON || error == 0) {
        t->json_error_count += 1;
        if (t->json_error_count >= _cfg.json_error_limit) {
            FailTransfer(t, llfc_proto::ERR_JSON,
                QString::fromLatin1("repeated malformed response"));
            return;
        }
        RollbackUploadTo(t, fallback);
        ScheduleBackoff(t);
        EmitUpdate(t);
        return;
    }
    //其余（2002/2103/2104/2108/2114 及未知码）：退避后 1507 重同步。
    RollbackUploadTo(t, fallback);
    ScheduleBackoff(t);
    EmitUpdate(t);
}

void TransferEngine::VerifySourceHashOn2117(ResourceTransferRuntime* t,
    qint64 server_offset)
{
    //2117=服务端判定整文件内容不符：先复核本地整文件哈希，
    //与 1503 声明不同→"源文件已变更"，本地直接失败。
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QFile f(t->upload.file_path);
    if (!f.open(QIODevice::ReadOnly)) {
        FailTransfer(t, 0, QString::fromLatin1("source file unreadable"));
        return;
    }
    char buf[64 * 1024];
    qint64 got = 0;
    while ((got = f.read(buf, sizeof(buf))) > 0) {
        hash.addData(buf, static_cast<int>(got));
    }
    if (got < 0) {
        FailTransfer(t, 0, QString::fromLatin1("source file read failed"));
        return;
    }
    const QString computed = QString::fromLatin1(hash.result().toHex());
    if (computed != t->upload.sha256.toLower()) {
        FailTransfer(t, llfc_proto::RS_FILE_CONTENT_MISMATCH,
            QString::fromLatin1("source file changed"));
        return;
    }
    //本地未变：整文件重传（服务端已删 .part，server_offset=0）。
    RollbackUploadTo(t, server_offset);
    ScheduleBackoff(t);
    EmitUpdate(t);
}

bool TransferEngine::EnsureSourceOpen(ResourceTransferRuntime* t)
{
    //延迟打开源文件的唯一入口：1508 确认 Pending（需要发字节）后才调用。
    //已打开直接复用；缺失/大小不符/open 失败→本地终态（不重试本地源问题）。
    if (t->source) {
        return true;
    }
    const QFileInfo fi(t->upload.file_path);
    if (!fi.exists() || !fi.isFile()) {
        FailTransfer(t, 0, QString::fromLatin1("source file missing"));
        return false;
    }
    if (fi.size() != t->total_bytes) {
        FailTransfer(t, 0, QString::fromLatin1("source file size mismatch"));
        return false;
    }
    t->source = new QFile(t->upload.file_path);
    if (!t->source->open(QIODevice::ReadOnly)) {
        CloseFiles(t);
        FailTransfer(t, 0, QString::fromLatin1("source file open failed"));
        return false;
    }
    return true;
}

void TransferEngine::HandleChunkUploadRsp(const QByteArray& payload)
{
    rc::ChunkUploadRsp rsp;
    if (!rc::DecodeChunkUploadRsp(payload, &rsp)) {
        return; //连 message_id 都不可读，无法定位传输
    }
    ResourceTransferRuntime* t = FindActive(TRANSFER_UPLOAD, rsp.message_id);
    if (!t) {
        return;
    }
    //1507 同步在途时 1506 为迟到残留：仅在有过期队列时排空。
    if (t->phase == TRANSFER_QUERYING && t->inflight.empty()
        && t->stale_offsets.empty()) {
        return;
    }
    //过期回包只排空，不触发回退/推进。
    if (!t->stale_offsets.empty() && rsp.offset == t->stale_offsets.front()) {
        t->stale_offsets.pop_front();
        return;
    }
    //回显 offset 与队首不一致：全部标过期并重发 1507 重同步。
    if (rsp.offset < 0 || t->inflight.empty()
        || rsp.offset != t->inflight.front().offset) {
        if (rsp.offset >= 0 && t->inflight.empty() && t->stale_offsets.empty()
            && rsp.offset < t->wire_bytes) {
            return; //纯迟到重复（回退窗口内的旧确认），安全忽略
        }
        ResyncUpload(t);
        EmitUpdate(t);
        return;
    }
    t->inflight.pop_front();
    if (!rsp.ok) {
        t->json_error_count += 1;
        if (t->json_error_count >= _cfg.json_error_limit) {
            FailTransfer(t, llfc_proto::ERR_JSON,
                QString::fromLatin1("repeated malformed response"));
            return;
        }
        ScheduleBackoff(t);
        EmitUpdate(t);
        return;
    }
    if (rsp.error != 0) {
        HandleUploadError(t, rsp.error, rsp.server_offset);
        return;
    }
    //成功：单调推进确认位，弹出队首并补发。
    t->json_error_count = 0;
    t->chunk_retry_count = 0;
    if (rsp.server_offset > t->acked_bytes) {
        t->acked_bytes = qMin(rsp.server_offset, t->total_bytes);
    }
    if (rsp.status == kStatusPublished) {
        CompleteTransfer(t, rsp.event_seq);
        return;
    }
    if (t->acked_bytes >= t->total_bytes) {
        //字节收齐但未发布：重发末片再触发校验/发布（服务端约定）。
        MaybeResendLastChunk(t);
        return;
    }
    if (t->phase != TRANSFER_ACTIVE) {
        t->phase = TRANSFER_ACTIVE;
    }
    Pump();
    EmitUpdate(t);
}

void TransferEngine::MaybeResendLastChunk(ResourceTransferRuntime* t)
{
    if (t->user_paused) {
        return; //暂停期间不发末片；Resume→1507→Pending 收齐会再走到这里
    }
    if (t->publish_retry_count >= _cfg.chunk_retry_limit) {
        //完成点反复失败：转退避，唤醒后 1507 再探状态。
        t->publish_retry_count = 0;
        ScheduleBackoff(t);
        EmitUpdate(t);
        return;
    }
    if (!t->source) {
        //防御：源未打开（正常流 Pending 分支已 EnsureSourceOpen）。
        FailTransfer(t, 0, QString::fromLatin1("source file missing"));
        return;
    }
    t->publish_retry_count += 1;
    const qint64 last_off =
        ((t->total_bytes + t->chunk_size - 1) / t->chunk_size - 1) * t->chunk_size;
    const qint64 len = t->total_bytes - last_off;
    if (!t->source->seek(last_off)) {
        FailTransfer(t, 0, QString::fromLatin1("local source seek failed"));
        return;
    }
    const QByteArray raw = t->source->read(len);
    if (raw.size() != len) {
        FailTransfer(t, 0, QString::fromLatin1("local source read failed"));
        return;
    }
    InflightChunk c;
    c.offset = last_off;
    c.length = len;
    c.sha256 = Sha256HexOf(raw);
    c.sent_ms = NowMs();
    t->inflight.push_back(c);
    if (t->phase != TRANSFER_ACTIVE) {
        t->phase = TRANSFER_ACTIVE;
    }
    if (!_send) {
        qWarning() << "[TransferEngine] no send callback, last chunk dropped msg="
                   << t->message_id;
        return;
    }
    _send(llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_REQ,
        rc::EncodeChunkUploadReq(t->message_id, last_off, c.sha256, raw));
    EmitUpdate(t);
}

void TransferEngine::HandleProgressRsp(const QByteArray& payload)
{
    rc::ProgressRsp rsp;
    if (!rc::DecodeProgressRsp(payload, &rsp)) {
        return;
    }
    ResourceTransferRuntime* t = FindActive(TRANSFER_UPLOAD, rsp.message_id);
    if (!t) {
        return;
    }
    if (t->proof_pending) {
        //本传输正在等 1514（1513 已发出）：此刻到达的 1508 只可能是上一轮
        //1507 的迟到/重复回包，按陈旧帧整帧丢弃——既不更新状态也不动 1513
        //的控制计时器（清掉它会让 1513 永远不会超时重连）。真值最终由该
        //1514（Published 幂等/2118）或 1513 超时后的 1507 重同步收敛。
        return;
    }
    t->query_sent_ms = 0;
    if (!rsp.ok || rsp.error != 0) {
        HandleUploadError(t, rsp.ok ? rsp.error : llfc_proto::ERR_JSON,
            rsp.server_offset);
        return;
    }
    if (rsp.status == kStatusPublished) {
        //服务端已发布（含本地源文件已删/改名的恢复场景）：直接完成，
        //不得因源文件问题误标 Failed。
        CompleteTransfer(t, rsp.event_seq);
        return;
    }
    if (rsp.status == kStatusFailed) {
        FailTransfer(t, llfc_proto::RS_RESOURCE_STATE_INVALID,
            QString::fromLatin1("server marked failed"));
        return;
    }
    if (rsp.file_size_bytes != t->total_bytes) {
        //1503 声明与服务端真值不符：元数据错（file_size_bytes 成功回包必在）。
        FailTransfer(t, 0, QString::fromLatin1("file size mismatch with server"));
        return;
    }
    //1508 chunk_size 如存在必须等于共享 wire 单位（256KiB）：不匹配即坏回包
    if (rsp.chunk_size > 0
        && rsp.chunk_size != static_cast<qint64>(llfc_wire::kResourceChunkSize)) {
        t->json_error_count += 1;
        if (t->json_error_count >= _cfg.json_error_limit) {
            FailTransfer(t, llfc_proto::ERR_JSON,
                QString::fromLatin1("server chunk_size mismatch"));
            return;
        }
        ScheduleBackoff(t); //退避后 1507 重同步
        EmitUpdate(t);
        return;
    }
    //全部成功字段与（proof 模式下）挑战参数都校验通过后才清零错误计数：
    //此前清零会让反复坏挑战永远到不了 json_error_limit（0→1→0→1 震荡），
    //客户端只会无限退避。普通上传路径（无挑战）在下面立即清零。
    qint64 so = rsp.server_offset >= 0 ? rsp.server_offset : 0;
    if (so > t->total_bytes) {
        so = t->total_bytes;
    }
    t->acked_bytes = so;
    t->wire_bytes = so;
    t->phase = TRANSFER_ACTIVE;
    if (t->user_paused) {
        //暂停期间不推进上传；字段合法但未清零连续错误计数（下一条匹配的
        //成功回包仍会清零，退避计数不在暂停期被无声抹除）。
        EmitUpdate(t);
        return;
    }
    //Pending：此刻才需要源文件字节——检查/打开源文件（延迟打开的统一入口），
    //缺失/大小不符在此失败。
    if (!EnsureSourceOpen(t)) {
        return; //EnsureSourceOpen 内部已 FailTransfer
    }
    //P2 秒传：服务端下发一次性挑战（同 sender Published 历史 + blob 存在
    // + 大小一致才下发）。2118 过的传输（proof_disabled）忽略挑战直接
    //普通上传——与服务端 challenge_suppressed 双端闭环，绝不无限挑战。
    if (rsp.mode == QStringLiteral("proof") && !t->proof_disabled) {
        //SendInstantProof 内部不再清零坏回包计数（合法挑战只是"新一次证明
        //尝试"，清零会让非法 1514 永远到不了 json_error_limit）；计数由普通
        //上传分支（下面）在字段全部通过后清零。本调用返回后 t 可能已被同步
        //回调释放，故立即 return，绝不在此函数内再触碰 t。
        SendInstantProof(t, rsp.challenge_nonce, rsp.challenge_offset,
            rsp.challenge_length);
        return;
    }
    //普通上传路径：1508 字段已全部校验通过（含 1513 前的挑战分支）
    t->json_error_count = 0;
    t->publish_retry_count = 0;
    if (so >= t->total_bytes) {
        //Pending 且收齐：不宣告完成也不停在空窗口，重发末片触发发布事务。
        MaybeResendLastChunk(t);
        return;
    }
    Pump();
    EmitUpdate(t);
}

void TransferEngine::SendInstantProof(ResourceTransferRuntime* t,
    const QString& nonce_hex, qint64 offset, qint64 length)
{
    //注意：调用方在返回后不得再触碰 t——内部 _send/EmitUpdate 可能同步
    //触发回调（Reset/替换传输表）释放 t；失败与终态都在本函数内处理完。
    //P2 秒传证明（File 线程，源文件只读）。严格校验挑战参数：nonce 必须是
    //**恰好 32 个 ASCII hex 字符**（0-9/a-f/A-F，大小写均可，解码后 16 原字节）；
    //区段必须在文件范围内（0<length≤64KiB，offset+length≤total）。非法挑战按
    //坏回包计。
    //逐字符校验必须在 fromHex **之前**：QByteArray::fromHex 会跳过非法字符，
    //并把奇数个有效 nibble 中"多出来的那一个"当作一个整字节写出（最左字节
    //只占低 nibble、高 nibble 为 0），返回长度仍是 (size+1)/2。因此
    //31 个有效 hex + 1 个非法字符照样解出 16 字节，"字符数 32 + 解码后 16 字节"
    //并不能证明输入严格是 32 hex。只有逐字符确认才能保证解码语义与声明一致。
    bool nonce_chars_ok = (nonce_hex.size() == 32);
    for (int i = 0; nonce_chars_ok && i < nonce_hex.size(); ++i) {
        const QChar c = nonce_hex.at(i);
        nonce_chars_ok = (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
            || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
            || (c >= QLatin1Char('A') && c <= QLatin1Char('F'));
    }
    const QByteArray nonce_raw = QByteArray::fromHex(nonce_hex.toLatin1());
    if (!nonce_chars_ok || nonce_raw.size() != 16
        || offset < 0 || length <= 0 || length > 64 * 1024
        || offset > t->total_bytes - length) {
        t->json_error_count += 1;
        if (t->json_error_count >= _cfg.json_error_limit) {
            FailTransfer(t, llfc_proto::ERR_JSON,
                QString::fromLatin1("malformed proof challenge"));
            return;
        }
        ScheduleBackoff(t); //退避后 1507 重同步
        EmitUpdate(t);
        return;
    }
    if (!t->source || !t->source->seek(offset)) {
        FailTransfer(t, 0, QString::fromLatin1("local source seek failed"));
        return;
    }
    const QByteArray section = t->source->read(length);
    if (section.size() != length) {
        FailTransfer(t, 0, QString::fromLatin1("local source read failed"));
        return;
    }
    //proof = hex(SHA256(nonce_raw16 || file[off, off+len)))——nonce 必须
    //hex 解码成 16 原字节再拼 raw 区段，绝不能拼 hex 字符串。
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(nonce_raw);
    h.addData(section);
    const QString proof = QString::fromLatin1(h.result().toHex());
    //进入证明状态前收敛窗口：QUERYING 的不变量是"无在途发送窗口"（Pump 只在
    //ACTIVE 发片）。正常流 1508 由 1507 触发、BeginQuery 已排空窗口；迟到的
    //重复 1508 可能带残留在途，统一转入过期队列（迟到 1506 只排空不推进），
    //与 1507 重同步走同一条处理。
    RollbackUploadTo(t, t->acked_bytes);
    if (!_send) {
        //引擎未接线属编程错误：直接终态（此时尚未改动在途状态，安全）。
        FailTransfer(t, llfc_proto::ERR_JSON,
            QString::fromLatin1("no send callback"));
        return;
    }
    //连续坏回包计数（json_error_count）**不在此清零**：本函数每次调用只是
    //"一次新的证明尝试"，一次合法的挑战 metadata 与一次合法 1514 都不代表
    //上一轮坏 1514 已经过去。此前在这里清零会让 STATUS/OFFSET/EVENT_SEQ 非法的
    //1514 形成 0→1→0→1 震荡：bad1514 → HandleUploadError(ERR_JSON) 计数+1 →
    //退避 → 1507/1508(合法 proof) → 本函数清零 → 再 bad1514，永远到不了
    //json_error_limit。清零只发生在真正进入普通上传之后（HandleProgressRsp 的
    //非 proof 分支：普通有效 1508 且字段全部通过），2118 回退后走的就是那条路。
    //publish_retry_count 保持原语义（合法 1508/挑战不代表发布重试仍在恶化）。
    //本函数后续对 t 的任何写入都必须在 _send 之前完成：_send 可能同步触发
    //回调（Reset/替换传输表）从而释放 t，调用方返回后不得再写 t。
    t->publish_retry_count = 0;
    //1513 是控制帧：保持 QUERYING 并挂 query 超时（与 1507/1509 一致）——
    //超时判连接死亡→重连→1507 重同步（挑战丢失/重启即在此恢复）。
    t->phase = TRANSFER_QUERYING;
    t->proof_pending = true; //只认匹配的 1514；陈旧应答一律丢弃
    t->query_sent_ms = NowMs();
    //_send 是同步回调：一次合法的 Published 1514 会在 _send 内部就被派发到
    //HandleInstantProofRsp → CompleteTransfer → sig_transfer_finished，监听者可
    //能 Reset()（_uploads.clear()）或 StartUpload 同 message_id（erase + 插入新
    //实例），前者释放 t、后者让 t 变成被替换的旧实例。因此发送前先在运行时表里
    //按 message_id 取出**同一个实例**的 shared_ptr 握住（确认 get() == t），
    //并缓存 mid；_send 返回后只经由这个 shared_ptr 回看状态，绝不直接用裸 t。
    const qint64 mid = t->message_id;
    RuntimeMap& uploads = MapFor(TRANSFER_UPLOAD);
    std::shared_ptr<ResourceTransferRuntime> hold;
    {
        RuntimeMap::iterator it = uploads.find(mid);
        if (it != uploads.end() && it.value().get() == t) {
            hold = it.value();
        }
    }
    _send(llfc_proto::MSG_RESOURCE_INSTANT_PROOF_REQ,
        rc::EncodeInstantProofReq(mid, proof));
    //只在"表里仍是同一实例、且仍在等证明"时才发进度：完成/回滚会清掉
    //proof_pending（1514 或 1507 重同步），Reset/同 mid 重启则会清空或替换表项，
    //两种情况下都不允许再发旧进度，也不允许触碰已释放的旧实例。
    if (!hold) {
        return; //表里已无 t（无法安全回看实例）：只发送，不发进度
    }
    RuntimeMap::iterator after = uploads.find(mid);
    if (after == uploads.end() || after.value().get() != hold.get()
        || !hold->proof_pending) {
        return; //已完成/已回滚/已被 Reset 或同 mid 替换：绝不发旧进度
    }
    EmitUpdate(hold.get());
}

void TransferEngine::HandleInstantProofRsp(const QByteArray& payload)
{
    rc::InstantProofRsp rsp;
    if (!rc::DecodeInstantProofRsp(payload, &rsp)) {
        return; //连 message_id 都不可读，无法定位传输
    }
    ResourceTransferRuntime* t = FindActive(TRANSFER_UPLOAD, rsp.message_id);
    if (!t) {
        return;
    }
    if (!t->proof_pending) {
        //没有在途 1513（已被 1507 重同步/暂停/重连取代，或本传输已完成）：
        //整帧丢弃。清 query_sent_ms 会抹掉当前 1507 的控制定时器，继续按
        //2118 回退则会把在线上的普通上传窗口回滚——陈旧应答绝不允许影响
        //当前阶段。
        return;
    }
    t->proof_pending = false; //匹配的应答：本阶段控制帧到期
    t->query_sent_ms = 0;
    if (!rsp.ok) {
        HandleUploadError(t, llfc_proto::ERR_JSON, -1);
        return;
    }
    if (rsp.error == llfc_proto::RS_INSTANT_PROOF_INVALID) { //2118
        //证明不符/挑战丢失/TTL 过期：本传输内禁用 proof（客户端防无限
        //挑战闸门），1507 重同步后按普通上传继续——服务端可能再发
        //mode=proof（新会话未抑制），HandleProgressRsp 据此忽略。
        t->proof_disabled = true;
        ResyncUpload(t);
        EmitUpdate(t);
        return;
    }
    if (rsp.error != 0) {
        //其余错误复用上传错误分类：2002 退避后 1507 重同步（服务端
        //Transient 保留了挑战，新 1507 会再发挑战）；2111/2115/2116 永久。
        HandleUploadError(t, rsp.error, rsp.server_offset);
        return;
    }
    if (rsp.status != kStatusPublished) {
        //成功但非 Published：无效成功包按坏回包计数（json_error_count 累计
        //→ 退避后 1507 重同步 → 达 json_error_limit 转永久失败），绝不当作
        //"正常状态切换"去无界立即重同步：反复 {error:0,status:Pending} 会在
        //这里被有界收敛（服务端真值由随后的 1507 探明）。
        HandleUploadError(t, llfc_proto::ERR_JSON, -1);
        return;
    }
    //严格成功字段：Published 秒传必为全量发布——server_offset 必须等于
    //本传输总长（不能靠服务端回包"看起来成功"就强行置满进度），且服务端
    //已为接收者分配事件序号（event_seq 为十进制字符串且 >0；缺失/JSON 数字/
    //0 已在解码器判为坏包走 !rsp.ok，这里再防一层）。任一不符按坏回包
    //处理（连续计数 + 退避后 1507 重同步，达到 json_error_limit 转永久
    //失败）；不用该回包的 server_offset 回退窗口（它已被判定不可信），
    //回退到已确认位。
    if (rsp.server_offset != t->total_bytes || rsp.event_seq <= 0) {
        HandleUploadError(t, llfc_proto::ERR_JSON, -1);
        return;
    }
    //秒传命中：与普通上传完成走同一条完成路径（snapshot → 终态信号 →
    //Outbox 确认 → 异步归档，由上层统一处理）。
    t->acked_bytes = t->total_bytes;
    t->wire_bytes = t->total_bytes;
    CompleteTransfer(t, rsp.event_seq);
}

void TransferEngine::HandleDownInfoRsp(const QByteArray& payload)
{
    rc::DownInfoRsp rsp;
    if (!rc::DecodeDownInfoRsp(payload, &rsp)) {
        return;
    }
    ResourceTransferRuntime* t = FindActive(TRANSFER_DOWNLOAD, rsp.message_id);
    if (!t) {
        return;
    }
    t->query_sent_ms = 0;
    if (!rsp.ok || rsp.error != 0) {
        const int error = rsp.ok ? rsp.error : llfc_proto::ERR_JSON;
        if (IsPermanentError(error)) {
            FailTransfer(t, error, QString::fromLatin1("permanent server error"));
            return;
        }
        if (error == llfc_proto::ERR_JSON) {
            t->json_error_count += 1;
            if (t->json_error_count >= _cfg.json_error_limit) {
                FailTransfer(t, error,
                    QString::fromLatin1("repeated malformed response"));
                return;
            }
        }
        //2002/2103/2104/2108/2114/未知：退避后重查 1509。
        ScheduleBackoff(t);
        EmitUpdate(t);
        return;
    }
    t->json_error_count = 0;
    if (t->download.file_size_bytes > 0
        && t->download.file_size_bytes != rsp.file_size_bytes) {
        FailTransfer(t, 0, QString::fromLatin1("metadata size mismatch"));
        return;
    }
    t->total_bytes = rsp.file_size_bytes;
    if (!rsp.sha256.isEmpty()) {
        t->expected_sha256 = rsp.sha256;
    }
    OpenDownloadPart(t);
}

void TransferEngine::OpenDownloadPart(ResourceTransferRuntime* t)
{
    delete t->part;
    t->part = new QFile(PartPathOf(t->download.dest_file_path));
    if (!t->part->open(QIODevice::ReadWrite)) {
        CloseFiles(t);
        FailTransfer(t, 0, QString::fromLatin1("part file open failed"));
        return;
    }
    qint64 size = t->part->size();
    if (size > t->total_bytes) {
        size = 0; //超过总长的 .part 不可信，从零重来
    }
    //续传前截断到分片边界：丢弃不完整的尾片，服务端按整片重发。
    const qint64 resume = size > 0
        ? ((size - 1) / t->chunk_size) * t->chunk_size : 0;
    if (resume != t->part->size() && !t->part->resize(resume)) {
        CloseFiles(t);
        FailTransfer(t, 0, QString::fromLatin1("part file truncate failed"));
        return;
    }
    t->acked_bytes = resume;
    t->wire_bytes = resume;
    delete t->hasher;
    t->hasher = new QCryptographicHash(QCryptographicHash::Sha256);
    t->hashed_len = 0;
    //从 0 连续写入才增量哈希；续传（resume>0）完成时回退整文件哈希。
    t->hash_valid = (resume == 0);
    t->inflight.clear();
    t->stale_offsets.clear();
    if (t->total_bytes == 0 || t->acked_bytes >= t->total_bytes) {
        FinishDownload(t); //0 字节资源或 .part 已收齐：直接校验收尾
        return;
    }
    t->phase = TRANSFER_ACTIVE;
    Pump();
    EmitUpdate(t);
}

void TransferEngine::HandleChunkDownRsp(const QByteArray& payload)
{
    rc::ChunkDownRsp rsp;
    if (!rc::DecodeChunkDownRsp(payload, &rsp)) {
        return;
    }
    ResourceTransferRuntime* t = FindActive(TRANSFER_DOWNLOAD, rsp.message_id);
    if (!t) {
        return;
    }
    if (t->phase == TRANSFER_QUERYING && t->inflight.empty()
        && t->stale_offsets.empty()) {
        return; //1509 在途，迟到 1512 忽略
    }
    if (!t->stale_offsets.empty() && rsp.offset == t->stale_offsets.front()) {
        t->stale_offsets.pop_front(); //过期回包只排空
        return;
    }
    if (!rsp.ok || rsp.error != 0) {
        const int error = rsp.ok ? rsp.error : llfc_proto::ERR_JSON;
        if (IsPermanentError(error)) {
            FailTransfer(t, error, QString::fromLatin1("permanent server error"));
            return;
        }
        if (error == llfc_proto::RS_FILE_OFFSET_INVALID) {
            //游标分叉：以本地 .part 为真值重新同步。
            t->inflight.clear();
            t->stale_offsets.clear();
            t->wire_bytes = t->acked_bytes;
            ScheduleBackoff(t);
            EmitUpdate(t);
            return;
        }
        if (error == llfc_proto::ERR_JSON) {
            t->json_error_count += 1;
            if (t->json_error_count >= _cfg.json_error_limit) {
                FailTransfer(t, error,
                    QString::fromLatin1("repeated malformed response"));
                return;
            }
        }
        ScheduleBackoff(t);
        EmitUpdate(t);
        return;
    }
    if (rsp.offset < 0 || t->inflight.empty()
        || rsp.offset != t->inflight.front().offset) {
        if (rsp.offset >= 0 && t->inflight.empty() && t->stale_offsets.empty()
            && rsp.offset < t->wire_bytes) {
            return; //迟到重复，安全忽略
        }
        //回显不一致：下载窗口作废，退避后重查 1509 重建。
        ScheduleBackoff(t);
        EmitUpdate(t);
        return;
    }
    const QByteArray& raw = rsp.data;
    //队首期望长度须在弹出前捕获（校验失败重取也依赖它）。
    const qint64 expected_len = t->inflight.front().length;
    t->inflight.pop_front();
    //逐片校验（成功回包全满足才允许写盘/推进）：
    //非空片；精确等长 raw.size==bytes==队首 expected.length；
    //total_size 与本地总长相等；offset+length 不越界；
    //哈希为合法 64 位小写 hex（codec 已保证格式）且与内容一致；
    //与已落盘位置连续。
    const bool len_ok = !raw.isEmpty() && raw.size() == rsp.bytes
        && raw.size() == expected_len;
    const bool total_ok = rsp.total_size == t->total_bytes;
    const bool range_ok = rsp.offset <= t->total_bytes - raw.size();
    const bool hash_ok = Sha256HexOf(raw) == rsp.chunk_sha256;
    const bool pos_ok = rsp.offset == t->acked_bytes;
    if (!len_ok || !total_ok || !range_ok || !hash_ok || !pos_ok) {
        t->chunk_retry_count += 1;
        if (t->chunk_retry_count >= _cfg.chunk_retry_limit) {
            FailTransfer(t, 0, QString::fromLatin1("chunk validation failed"));
            return;
        }
        //该片作废重取：其后在途转过期（先到者排空），窗口回退到该片偏移。
        while (!t->inflight.empty()) {
            t->stale_offsets.push_back(t->inflight.front().offset);
            t->inflight.pop_front();
        }
        t->wire_bytes = rsp.offset;
        Pump();
        EmitUpdate(t);
        return;
    }
    t->json_error_count = 0;
    t->chunk_retry_count = 0;
    if (!t->part || !t->part->seek(rsp.offset)) {
        FailTransfer(t, 0, QString::fromLatin1("part file seek failed"));
        return;
    }
    if (t->part->write(raw) != raw.size()) {
        FailTransfer(t, 0, QString::fromLatin1("part file write failed"));
        return;
    }
    //从 0 连续写入时增量哈希；一旦跳写即失效，完成时整文件重算。
    if (t->hash_valid && t->hashed_len == rsp.offset) {
        t->hasher->addData(raw);
        t->hashed_len += raw.size();
    } else {
        t->hash_valid = false;
    }
    t->acked_bytes = rsp.offset + raw.size();
    if (t->acked_bytes >= t->total_bytes) {
        FinishDownload(t);
        return;
    }
    Pump();
    EmitUpdate(t);
}

void TransferEngine::FinishDownload(ResourceTransferRuntime* t)
{
    if (!t->part) {
        FailTransfer(t, 0, QString::fromLatin1("part file missing"));
        return;
    }
    t->part->flush();
    QByteArray final_hash;
    if (t->hash_valid) {
        final_hash = t->hasher->result();
    } else {
        //续传路径：整文件哈希兜底。
        QCryptographicHash whole(QCryptographicHash::Sha256);
        if (!t->part->seek(0)) {
            FailTransfer(t, 0, QString::fromLatin1("part file seek failed"));
            return;
        }
        char buf[64 * 1024];
        qint64 got = 0;
        while ((got = t->part->read(buf, sizeof(buf))) > 0) {
            whole.addData(buf, static_cast<int>(got));
        }
        if (got < 0) {
            FailTransfer(t, 0, QString::fromLatin1("part file read failed"));
            return;
        }
        final_hash = whole.result();
    }
    if (!t->expected_sha256.isEmpty()
        && QString::fromLatin1(final_hash.toHex())
            != t->expected_sha256.toLower()) {
        //整文件不符：.part 不可信，删除后终态（下次 Start 从头）。
        t->part->close();
        QFile::remove(PartPathOf(t->download.dest_file_path));
        FailTransfer(t, 0, QString::fromLatin1("whole file hash mismatch"));
        return;
    }
    const QString dest = t->download.dest_file_path;
    t->part->close();
    if (QFile::exists(dest) && !QFile::remove(dest)) {
        FailTransfer(t, 0, QString::fromLatin1("dest file remove failed"));
        return;
    }
    if (!QFile::rename(PartPathOf(t->download.dest_file_path), dest)) {
        FailTransfer(t, 0, QString::fromLatin1("part rename failed"));
        return;
    }
    t->acked_bytes = t->total_bytes;
    t->local_download_path = dest; //快照携带下载完成路径（GUI 据此落库）
    CompleteTransfer(t, 0);
}

void TransferEngine::CompleteTransfer(ResourceTransferRuntime* t,
    qint64 event_seq)
{
    t->phase = TRANSFER_COMPLETED;
    t->event_seq = event_seq;
    t->error_code = 0;
    t->error_text.clear();
    CloseFiles(t);
    const TransferSnapshot s = MakeSnapshot(t);
    //方向先取出：sig_transfer_finished 的槽可能与引擎同线程同步执行
    //（Reset/重新 StartUpload 同一 message_id 会 erase 本对象），emit 之后
    //再读 t->方向是 use-after-free。
    const int direction = t->direction;
    emit sig_transfer_finished(s);
    PromoteWaiting(direction);
}

void TransferEngine::FailTransfer(ResourceTransferRuntime* t, int error_code,
    const QString& text)
{
    t->phase = TRANSFER_FAILED;
    t->error_code = error_code;
    t->error_text = text;
    CloseFiles(t);
    const TransferSnapshot s = MakeSnapshot(t);
    //同上：emit 后不再解引用 t（同步槽可能已 Reset/重启同一 message_id）。
    const int direction = t->direction;
    emit sig_transfer_finished(s);
    PromoteWaiting(direction);
}

TransferSnapshot TransferEngine::MakeSnapshot(
    const ResourceTransferRuntime* t) const
{
    TransferSnapshot s;
    s.message_id = t->message_id;
    s.direction = t->direction;
    //暂停为独立意图：内部 phase 照常推进，快照对非终态统一投影 PAUSED，
    //方向/阶段对外始终自洽。
    s.phase = (t->user_paused && t->phase != TRANSFER_COMPLETED
        && t->phase != TRANSFER_FAILED) ? TRANSFER_PAUSED : t->phase;
    s.acked_bytes = t->acked_bytes;
    s.sent_bytes = t->wire_bytes;
    s.total_bytes = t->total_bytes;
    s.event_seq = t->event_seq;
    s.error_code = t->error_code;
    s.error_text = t->error_text;
    s.download_local_path = t->local_download_path;
    return s;
}

void TransferEngine::EmitUpdate(ResourceTransferRuntime* t)
{
    emit sig_transfer_updated(MakeSnapshot(t));
}
