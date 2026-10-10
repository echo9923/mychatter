#ifndef TRANSFERENGINE_H
#define TRANSFERENGINE_H

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QTimer>
#include <functional>
#include <memory>

#include "resourcetransfer.h"
//线协议分片单位唯一真值（全局 include，任何命名空间之前）
#include "resource_wire.h"

//引擎内部运行时状态（仅 transferengine.cpp 可见完整定义）。
//注意不得与 global.h 的 enum class TransferState 同名（FileTcpMgr/GUI 会
//同时包含两者），故命名 ResourceTransferRuntime。
struct ResourceTransferRuntime;

class QFile;
class QCryptographicHash;

//TransferEngine：资源上传/下载状态机内核（P1b-01，纯 QtCore，不引用 global.h）。
//
//定位：不建 socket、不碰 UI/outbox——通过 SetSendCallback 注入的 send 回调
//发帧，传输层把 1506/1508/1510/1512 回包经 OnFrame 喂回来，因此不建连接即可
//做单元测试；P1b-02 由 FileTcpMgr 接入。
//
//线程模型：实例驻留 File 线程，所有公有方法必须在该线程直接调用（跨线程由
//上层信号包装投递）。窗口/超时/退避/调度状态全部为本引擎单一所有者；内部
//唯一 QTimer（tick_interval_ms，默认 250ms）父挂 this。快照为跨线程值对象。
//
//方向语义：上传/下载登记表按方向分立——同一 message_id 的上传终态不会吞掉
//后续 StartDownload（重新下载已删缓存），反之亦然；对外 Pause/Resume/Cancel/
//GetSnapshot 均显式携带方向，防 UI 误路由。
//
//语义（与计划 P1b 引擎语义逐条对应）：
// - 以 message_id 为 key（按方向分表）；同时最多 max_active_uploads 个上传、
//   max_active_downloads 个下载活跃，超出的排队等待槽位。
// - 每传输最多 max_inflight_chunks 片在途，FIFO；每个 1506/1512 先比对回显
//   offset 与队首：一致→正常推进（acked=max(acked,server_offset) 后补片）；
//   2107/2112→回退到 server_offset 并把在途全部标为过期（过期回包只排空）；
//   不一致→全部标过期并重发 1507 同步。
// - 分片 SHA-256 在发片时现算（不预计算整表）。
// - 错误分类：永久 2111/2113/2115/2116/连续 2001；退避重试 1s→30s
//   （2002/2103/2104/2108/2114）；2117 先复核本地整文件哈希，与声明不符则
//   本地判"源文件已变更"。
// - 在途队首与 1507/1509 查询均受 ack_timeout_ms（默认 30s）约束，超时视为
//   连接已死，发 sig_request_reconnect。
// - 上传启动先发 1507 确认服务端状态，再校验源文件存在/大小：已 Published 而
//   本地源已删/改名→仍按已完成收尾（不误标 Failed）；仅 Pending 需要发字节时
//   才因源文件缺失/变更失败。
// - 1508 Pending 且 server_offset==file_size：不宣告完成也不停在空窗口，重发
//   最后一个非空分片触发服务端发布事务，直到 1506/1508 Published 才终态。
// - 重复 Start 同一 message_id：非终态（活跃）直接忽略；终态记录允许重新启动。
// - 下载：持久打开的 QFile 写 "<dest>.part"，续传前截断到分片边界；从 0 连续
//   落盘时增量哈希，否则完成时回退整文件哈希；.part 已达总长直接校验收尾；
//   完成后 rename 到目标路径。
// - 调度：按在途字节数公平补片（在途字节最少的活跃传输先补）。
class TransferEngine : public QObject {
    Q_OBJECT
public:
    struct Config {
        int max_active_uploads = 2;
        int max_active_downloads = 4;
        int max_inflight_chunks = 4;   //每传输在途分片上限（FIFO 窗口）
        int ack_timeout_ms = 30000;    //在途队首/查询无回包的连接死亡判定
        int backoff_initial_ms = 1000; //暂时错误退避起点
        int backoff_max_ms = 30000;    //退避上限
        int json_error_limit = 3;      //连续 2001/坏回包转永久的阈值
        int chunk_retry_limit = 3;     //单分片连续重试上限（下载校验/2112）
        //P1c：wire 分片单位唯一真值 = llfc_wire::kResourceChunkSize（256KiB）。
        //不可配置——旧 32KiB 配置会造成客户端窗口与 1512 raw 分片错位。
        //1508 如带 chunk_size，必须等于该值，不匹配按坏回包处理。
        qint64 chunk_size = static_cast<qint64>(llfc_wire::kResourceChunkSize);
        int tick_interval_ms = 250;    //tick 周期（测试可调小以加速超时/退避）
    };

    //msg_id 数值唯一来源 proto/protocol_ids.h（llfc_proto 命名空间）。
    typedef std::function<void(quint16 msg_id, const QByteArray& payload)> SendFn;

    //C++11：避免"类内默认实参 = 带默认成员初始化的 Config()"的编译器差异。
    explicit TransferEngine(QObject* parent = 0);
    explicit TransferEngine(const Config& config, QObject* parent = 0);
    ~TransferEngine();

    //注入发帧通道（引擎线程调用；未注入时发帧仅告警丢弃，靠超时兜底）。
    void SetSendCallback(SendFn send);

    //启动传输：同方向同 message_id 非终态→直接忽略（幂等，返回 true）；
    //终态记录允许重新启动。本地参数校验失败（路径空/大小非法）→登记 Failed
    //并发终态信号，返回 false。源文件缺失/大小不符不在此失败：先 1507 确认
    //服务端状态，仅 Pending 需要发字节时才失败。
    bool StartUpload(const UploadRequest& req);
    bool StartDownload(const DownloadRequest& req);

    //暂停=独立意图（user_paused），覆盖 Queued/Querying/Backoff/Active：
    //已授槽保留、排队者保持队首资格；暂停期间抑制一切发片与 1507/1509
    //控制补发，断开重连不解除；迟到 1508/1510 只保存元数据或按 Published
    //完成。恢复按当前阶段重同步（上传 1507 / 下载 1509 重建），受 2/4
    //槽上限约束，不越槽。direction 取 TransferDirection 枚举值。
    void PauseTransfer(int direction, qint64 message_id);
    void ResumeTransfer(int direction, qint64 message_id);
    //用户取消：关闭文件、置 Failed(error_code=0, "cancelled") 并发终态信号。
    void CancelTransfer(int direction, qint64 message_id);

    //当前快照（引擎线程调用）；不存在或已终态被重启覆盖返回 false。
    bool GetSnapshot(int direction, qint64 message_id, TransferSnapshot* out) const;

    //传输层生命周期：断开→清在途（回包不会再到达）；1502 鉴权成功→活跃上传
    //重发 1507、活跃下载重发 1509（鉴权缓存已失效）。
    void OnConnectionDown();
    void OnConnectionAuthenticated();

    //静默重置（用户代次切换/主动登出）：关闭全部 QFile、清空方向登记表/
    //等待队列/在途窗口，不发任何信号。自动网络断开请用 OnConnectionDown。
    void Reset();

    //回包入口：1506/1508/1510/1512，其余 id 忽略。
    void OnFrame(quint16 msg_id, const QByteArray& payload);

signals:
    //每次进度/阶段变化（含窗口推进）。
    void sig_transfer_updated(TransferSnapshot snapshot);
    //终态一次：Completed 或 Failed。
    void sig_transfer_finished(TransferSnapshot snapshot);
    //ack 超时判定连接已死，请求传输层重连（重连成功后 OnConnectionAuthenticated）。
    void sig_request_reconnect();

private:
    //实现细节全部在 transferengine.cpp。
    typedef QHash<qint64, std::shared_ptr<ResourceTransferRuntime> > RuntimeMap;
    const RuntimeMap& MapFor(int direction) const;
    RuntimeMap& MapFor(int direction);

    void OnTick();
    bool SendNextChunk(ResourceTransferRuntime* t);
    void Pump();
    int ActiveCount(int direction) const;
    void PromoteWaiting(int direction);
    void BeginQuery(ResourceTransferRuntime* t);
    void HandleTimeout(ResourceTransferRuntime* t);
    ResourceTransferRuntime* FindActive(int direction, qint64 message_id);
    void HandleChunkUploadRsp(const QByteArray& payload);
    void HandleProgressRsp(const QByteArray& payload);
    void HandleDownInfoRsp(const QByteArray& payload);
    void HandleChunkDownRsp(const QByteArray& payload);
    void HandleUploadError(ResourceTransferRuntime* t, int error,
        qint64 server_offset);
    void RollbackUploadTo(ResourceTransferRuntime* t, qint64 server_offset);
    void ResyncUpload(ResourceTransferRuntime* t);
    void ScheduleBackoff(ResourceTransferRuntime* t);
    qint64 BackoffIntervalMs(int retry_count) const;
    void MaybeResendLastChunk(ResourceTransferRuntime* t);
    void VerifySourceHashOn2117(ResourceTransferRuntime* t, qint64 server_offset);
    void OpenDownloadPart(ResourceTransferRuntime* t);
    void FinishDownload(ResourceTransferRuntime* t);
    void CompleteTransfer(ResourceTransferRuntime* t, qint64 event_seq);
    void FailTransfer(ResourceTransferRuntime* t, int error_code,
        const QString& text);
    void CloseFiles(ResourceTransferRuntime* t);
    TransferSnapshot MakeSnapshot(const ResourceTransferRuntime* t) const;
    void EmitUpdate(ResourceTransferRuntime* t);
    qint64 NowMs() const;
    bool EnsureSourceOpen(ResourceTransferRuntime* t); //延迟打开源文件唯一入口
    void InitConfig(const Config& config); //下限规整 + metatype 注册 + tick 启动
    void CloseAll();                       //析构收口：关闭全部登记的文件句柄

    Config _cfg;
    SendFn _send;
    RuntimeMap _uploads;   //message_id → 上传运行时（方向分立，终态可被重启覆盖）
    RuntimeMap _downloads; //message_id → 下载运行时
    QQueue<qint64> _upload_wait;    //槽位等待队列（插入序 FIFO）
    QQueue<qint64> _download_wait;
    QTimer* _tick;                  //tick：超时检查 + 退避唤醒
    QElapsedTimer _clock;           //单调钟（测试可把超时/tick 调小）
    bool _conn_up;                  //连接存在且 1502 鉴权成功
    bool _awaiting_reconnect;       //已请求重连，避免重复发信号
};

#endif // TRANSFERENGINE_H
