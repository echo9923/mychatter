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
//SEND_TEXT→1017；SEND_IMAGE metadata→1035，1036→uploading 启动 FileTcpMgr 上传，
//1038 完成→confirmImageSent；DELIVERY_ACK→聚合 1049，1050→删条目。
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
    //TcpMgr 转发的 1036 图片元数据回包（含冲突/transient）
    void slot_img_msg_meta_rsp(int error, QString unique_id, QString unique_name,
        qint64 message_id, qint64 thread_id, qint64 fromuid, qint64 touid);
    //TcpMgr 转发的 1050 ACK 回包
    void slot_delivery_ack_rsp(int error, QList<qint64> message_ids);
    //FileTcpMgr 上传完成（1038/1044 收全）
    void slot_img_upload_done(QString unique_name);
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
    //按条目类型派发一次（文本 1017 / 图片 1035 / ACK 聚合 1049）
    void dispatchDueEntries();
    void dispatchImageEntry(const OutboxEntryDTO& entry);
    //重建 MsgInfo 并按阶段启动既有 FileTcpMgr 上传/1043 续传；源文件丢失→markSendFailed
    void startImageUpload(const OutboxEntryDTO& entry, qint64 server_message_id, bool resume);
    //指数退避推进（2s→30s），内存镜像 + 持久 next_retry_at
    void scheduleRetry(OutboxEntryDTO& entry);
    void removeEntry(const QString& dedup_key);

    QTimer* _scan_timer;                    //250ms 扫描定时器，parent 到 this
    QMap<QString, OutboxEntryDTO> _entries; //dedup_key → entry（只在 TCP 线程访问）
    QMap<QString, QString> _uploading;      //unique_name → client_message_id（上传中）
    QSet<QString> _resume_pending;          //等待 getMessageByClientId 回查的恢复项
    qint64 _retry_initial_ms;               //配置 RequestRetryInitialMs
    qint64 _retry_max_ms;                   //配置 RequestRetryMaxMs
    bool _connected;
};

#endif // OUTBOXDISPATCHER_H
