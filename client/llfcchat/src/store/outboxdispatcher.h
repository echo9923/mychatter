#ifndef OUTBOXDISPATCHER_H
#define OUTBOXDISPATCHER_H

#include <QObject>
#include <QTimer>
#include <QMap>
#include <QSet>
#include "singleton.h"
#include "localmessageDTO.h"

//登记项当前等待的本地事务。条目只有在对应事务 commit 成功后
//（结果信号 ok=true）才允许改变内存状态或离开登记簿。
enum PendingStoreAction {
    PENDING_NONE = 0,           //网络发送阶段（首发/退避重试 1301/1503）
    PENDING_CONFIRM_TEXT,       //1302 成功 → messages=sent + 删 outbox
    PENDING_CONFIRM_RESOURCE,   //1506/1508 Ready → messages=sent + 删 outbox
    PENDING_MARK_FAILED,        //永久失败 → messages=failed + 删 outbox
    PENDING_ADVANCE_STAGE       //1504 成功 → outbox.stage=uploading（不删条目）
};

//运行时登记项：entry 为持久化真值快照，其余为纯运行时状态（重启归零）
struct RuntimeEntry {
    OutboxEntryDTO entry;
    int retry_count = 0;            //退避重试计数（不落盘，重启从小退避重计）
    qint64 next_retry_at = 0;       //下次动作时刻（epoch ms，0=立即）
    PendingStoreAction action = PENDING_NONE;
    qint64 server_message_id = 0;   //资源运行时已知（1504/恢复回查），重连免查库
    QString chat_time;              //PENDING_CONFIRM_TEXT 参数
};

//OutboxDispatcher：驻留 TCP 线程（与 TcpMgr 同线程，独立 QObject）。
//单向状态模型：SQLite outbox 是唯一真值，_entries 只是运行时登记簿——
//条目进入登记簿的前提是 enqueueSend 事务已 commit（sig_send_enqueued 携带
//实际插入的 outbox 行增量登记，不做全表查询）；离开登记簿的前提是销账/推进
//事务已 commit（结果信号 ok=true 后才删内存）。断线不清登记簿，进程内重连
//按内存继续；全表 loadOutbox 仅用于冷启动/换用户恢复（sig_db_opened 作废
//旧登记簿，slot_start 触发一次快照恢复，恢复期间暂停派发）。
//SEND_TEXT→1301；SEND_RESOURCE metadata→1503，1504 成功（stage 落库提交后）
//→uploading 并向 ResourceServer 发 1507 查服务端真实偏移，1508 对齐后
//FileTcpMgr 窗口续发 1505，1506 resource_status=1 → confirmResourceSent。
//重试退避（retry_count/next_retry_at）为纯内存状态，不落盘。
class OutboxDispatcher : public QObject, public Singleton<OutboxDispatcher>,
        public std::enable_shared_from_this<OutboxDispatcher>
{
    Q_OBJECT
public:
    friend class Singleton<OutboxDispatcher>;
    ~OutboxDispatcher();
    //登录 1102/重连成功后恢复登记簿（公有 API 只 emit 信号，TCP 线程执行）
    void start();
public slots:
    //TcpMgr 转发的 1302 文本回包（含冲突/transient）
    void slot_text_msg_rsp(int error, QString unique_id, qint64 message_id,
        QString chat_time);
    //TcpMgr 转发的 1504 资源消息创建回包（含冲突/transient/permanent）
    void slot_resource_msg_meta_rsp(int error, QString unique_id, QString file_name,
        qint64 message_id, qint64 thread_id, qint64 fromuid, qint64 touid);
    //FileTcpMgr 资源上传收全（1506 resource_status=1）
    void slot_resource_upload_done(QString unique_name);
    //FileTcpMgr 资源上传永久失败（2115/2116 等）
    void slot_resource_upload_failed(QString unique_name, int error);
    //FileTcpMgr 转发的 1508 上传进度回包（恢复续传的统一对齐点）
    void slot_upload_progress_rsp(qint64 message_id, int error, qint64 server_offset,
        int resource_status);
    //TcpMgr 断线通知：停止扫描，登记簿保留等重连
    void slot_connection_closed();
private slots:
    void slot_start();
    void slot_scan_timeout();
    //CommittedSend：enqueueSend 事务 commit 成功后的增量登记
    void slot_send_enqueued_registered(bool ok, LocalMessageDTO dto, OutboxEntryDTO entry);
    //本地事务结果——销账/推进 commit 成功后才动登记簿
    void slot_send_confirmed(bool ok, LocalMessageDTO dto);
    void slot_send_failed_marked(bool ok, LocalMessageDTO dto);
    void slot_resource_confirmed(bool ok, LocalMessageDTO dto);
    void slot_resource_stage_updated(bool ok, LocalMessageDTO dto);
    //冷启动/换用户恢复：全表快照登记（仅此一处消费全量查询）
    void slot_outbox_loaded(bool ok, QList<OutboxEntryDTO> entries);
    //uploading 恢复时回查 server_message_id
    void slot_message_loaded(bool ok, LocalMessageDTO dto);
    //用户库打开/切换：旧登记簿整体作废，待 slot_start 从新库恢复
    void slot_db_opened(bool ok, int uid);
    void slot_db_closed();
    //ResourceServer 连接与鉴权独立于 ChatServer；1507/1505 只能在鉴权成功后发送
    void slot_resource_login_success();
    void slot_resource_login_failed(QString reason);
    void slot_resource_connection_closed();
signals:
    void sig_start();
private:
    OutboxDispatcher();
    void loadRetryConfig();
    //Chat 已登录且当前用户库已打开后才允许发起一次冷恢复；失败按退避重试
    void requestRestore();
    //登记簿到点动作：action != NONE 幂等重发本地事务；否则首发 1301/1503
    void dispatchDueEntries();
    //恢复完成后的首轮：按 operation_id（落库顺序）首发，维持恢复发送顺序
    void dispatchRestoredFirstRound(const QList<OutboxEntryDTO>& order);
    //网络首发（文本 1301 / 资源 metadata 1503；源文件丢失→MARK_FAILED）
    void dispatchTextEntry(RuntimeEntry& rt);
    void dispatchResourceEntry(RuntimeEntry& rt);
    //发起/重发本地事务（幂等：目标行缺失时为 no-op）
    void issuePendingAction(RuntimeEntry& rt);
    //登记一条动作、立即发起事务并安排退避重试
    void beginAction(const QString& key, RuntimeEntry& rt, PendingStoreAction action);
    //动作事务 commit 确认后删除登记（连带清理 _uploading 会话簿记）
    void eraseEntry(const QString& key);
    //重建 MsgInfo 并发 1507 查询服务端偏移（首传/续传统一入口）；
    //源文件丢失→MARK_FAILED
    void startResourceUpload(RuntimeEntry& rt, qint64 server_message_id);
    //1508 回包对齐 MsgInfo 后驱动窗口续发
    void resumeUploadByProgress(qint64 message_id, qint64 server_offset, int resource_status);
    //指数退避推进（2s→30s），纯内存
    void scheduleRetry(RuntimeEntry& rt);
    //资源发送失败收尾（映射 file_name → client_message_id）
    void failResourceByName(const QString& unique_name);
    //进程内重连：uploading 条目按登记簿恢复（server_message_id 已知免查库）
    void resumeUploadingEntries();

    QTimer* _scan_timer;                    //250ms 扫描定时器，parent 到 this
    QMap<QString, RuntimeEntry> _entries;   //dedup_key → 运行时登记（只在 TCP 线程访问）
    QMap<QString, QString> _uploading;      //file_name → client_message_id（上传中）
    QSet<QString> _resume_pending;          //等待 getMessageByClientId 回查的恢复项
    qint64 _retry_initial_ms;               //配置 RequestRetryInitialMs
    qint64 _retry_max_ms;                   //配置 RequestRetryMaxMs
    qint64 _restore_retry_at;               //恢复查询失败后的下次尝试时刻
    bool _connected;
    bool _db_ready;                         //当前用户 SQLite 已成功打开
    bool _resource_ready;                   //ResourceServer 1502 鉴权成功
    bool _restored;                         //当前用户登记簿是否已从库恢复
    bool _restoring;                        //恢复快照在途（暂停派发）
    int _active_uid;                        //登记簿所属用户，防同 uid 重复 open 清空运行态
};

#endif // OUTBOXDISPATCHER_H
