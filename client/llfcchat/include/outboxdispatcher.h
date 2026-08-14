#ifndef OUTBOXDISPATCHER_H
#define OUTBOXDISPATCHER_H

#include <QObject>
#include <QTimer>
#include <QMap>
#include <QSet>
#include "singleton.h"
#include "localmessageDTO.h"

//OutboxDispatcher：驻留 TCP 线程（与 TcpMgr 同线程，独立 QObject）。
//QTimer 扫描内存镜像 + 事件触发；SQLite outbox 为跨重启持久真值，内存为运行时镜像。
//SEND_TEXT→1017；SEND_RESOURCE metadata→1035，1036→uploading 后向 ResourceServer 发
//1041 查服务端真实偏移，1042 对齐后 FileTcpMgr 窗口续发 1037，1038 resource_status=1
//→confirmResourceSent；DELIVERY_ACK→聚合 1049，1050→删条目。
//重启/断线恢复统一走 1041：无需区分首传/续传（1043 分支已废弃）。
class OutboxDispatcher : public QObject, public Singleton<OutboxDispatcher>,
        public std::enable_shared_from_this<OutboxDispatcher>
{
    Q_OBJECT
public:
    friend class Singleton<OutboxDispatcher>;
    ~OutboxDispatcher();
    //登录 1006/重连成功后恢复队列（公有 API 只 emit 信号，TCP 线程执行）
    void start();
    //GUI 入库成功后触发一次立即派发
    void notifySendEnqueued(const LocalMessageDTO& dto);
public slots:
    //TcpMgr 转发的 1018 文本回包（含冲突/transient）
    void slot_text_msg_rsp(int error, QString unique_id, qint64 message_id, QString chat_time);
    //TcpMgr 转发的 1036 资源消息创建回包（含冲突/transient/permanent）
    void slot_resource_msg_meta_rsp(int error, QString unique_id, QString file_name,
        qint64 message_id, qint64 thread_id, qint64 fromuid, qint64 touid);
    //TcpMgr 转发的 1050 ACK 回包
    void slot_delivery_ack_rsp(int error, QList<qint64> message_ids);
    //FileTcpMgr 资源上传收全（1038 resource_status=1）
    void slot_resource_upload_done(QString unique_name);
    //FileTcpMgr 资源上传永久失败（1026/1027 等）
    void slot_resource_upload_failed(QString unique_name, int error);
    //FileTcpMgr 转发的 1042 上传进度回包（恢复续传的统一对齐点）
    void slot_upload_progress_rsp(qint64 message_id, int error, qint64 server_offset,
        int resource_status);
    //TcpMgr 断线通知：停止扫描等待重连
    void slot_connection_closed();
private slots:
    void slot_start();
    void slot_notify_send_enqueued(LocalMessageDTO dto);
    void slot_scan_timeout();
    void slot_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries);
    //uploading 恢复时回查 server_message_id
    void slot_message_loaded(bool ok, LocalMessageDTO dto);
    //1019/1039 落库成功后触发一次 ACK 立即派发
    void slot_incoming_inserted(bool ok, QList<LocalMessageDTO> msgs, QList<qint64> insertedIds);
signals:
    void sig_start();
    void sig_notify_send_enqueued(LocalMessageDTO dto);
private:
    OutboxDispatcher();
    void loadRetryConfig();
    //按条目类型派发一次（文本 1017 / 资源 1035 / ACK 聚合 1049）
    void dispatchDueEntries();
    void dispatchResourceEntry(const OutboxEntryDTO& entry);
    //重建 MsgInfo 并发 1041 查询服务端偏移（首传/续传统一入口）；
    //源文件丢失→markSendFailed
    void startResourceUpload(const OutboxEntryDTO& entry, qint64 server_message_id);
    //1042 回包对齐 MsgInfo 后驱动窗口续发
    void resumeUploadByProgress(qint64 message_id, qint64 server_offset, int resource_status);
    //指数退避推进（2s→30s），内存镜像 + 持久 next_retry_at
    void scheduleRetry(OutboxEntryDTO& entry);
    void removeEntry(const QString& dedup_key);
    //资源发送失败收尾（映射 file_name → client_message_id）
    void failResourceByName(const QString& unique_name);

    QTimer* _scan_timer;                    //250ms 扫描定时器，parent 到 this
    QMap<QString, OutboxEntryDTO> _entries; //dedup_key → entry（只在 TCP 线程访问）
    QMap<QString, QString> _uploading;      //file_name → client_message_id（上传中）
    QSet<QString> _resume_pending;          //等待 getMessageByClientId 回查的恢复项
    qint64 _retry_initial_ms;               //配置 RequestRetryInitialMs
    qint64 _retry_max_ms;                   //配置 RequestRetryMaxMs
    bool _connected;
};

#endif // OUTBOXDISPATCHER_H
