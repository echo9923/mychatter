#ifndef CHATSYNCMANAGER_H
#define CHATSYNCMANAGER_H

#include <QObject>
#include <QJsonObject>
#include "singleton.h"
#include "localmessageDTO.h"
#include "userdata.h"

//ChatSyncManager：驻留 TCP 线程（与 TcpMgr 同线程，独立 QObject）。
//登录 1006/重连成功后启动；bootstrap_complete=0 先走 bootstrap（1051 取 checkpoint
//→ 1025/1026 拿会话列表 → 写库 → MarkBootstrapComplete → 进入增量）；
//增量 1051 {after_sync_seq, limit:100}，1052 整页交 applySyncPage，
//提交成功信号返回后才推进游标，has_more 连拉。
class ChatSyncManager : public QObject, public Singleton<ChatSyncManager>,
        public std::enable_shared_from_this<ChatSyncManager>
{
    Q_OBJECT
public:
    friend class Singleton<ChatSyncManager>;
    ~ChatSyncManager();
    //登录 1006/重连成功后启动（公有 API 只 emit 信号，TCP 线程执行）
    void start();
    //1019/1039 落库成功后兜底触发一次增量拉取
    void notifyOnlineMessage();
public slots:
    //TcpMgr 转发的 1052 增量同步回包（原始 JSON 对象）
    void slot_sync_message_rsp(QJsonObject rsp);
    //TcpMgr 转发的 1026 会话列表回包（bootstrap 用）
    void slot_load_chat_thread(bool load_more, qint64 next_last_id,
        std::vector<std::shared_ptr<ChatThreadInfo>> chat_threads);
private slots:
    void slot_start();
    void slot_notify_online_message();
    void slot_sync_state_loaded(bool ok, qint64 lastSyncSeq, bool bootstrapComplete);
    void slot_conversations_upserted(bool ok);
    void slot_bootstrap_marked(bool ok, qint64 checkpoint);
    void slot_sync_page_applied(bool ok, qint64 newSyncSeq, QList<LocalMessageDTO> msgs,
        QList<qint64> insertedIds);
    void slot_connection_closed();
signals:
    void sig_start();
    void sig_notify_online_message();
private:
    ChatSyncManager();
    //同步状态机
    enum SyncState {
        SYNC_IDLE = 0,          //空闲（可发起增量拉取）
        SYNC_WAIT_STATE,        //等待 getSyncState 结果
        SYNC_WAIT_BOOTSTRAP,    //等待 bootstrap checkpoint 回包
        SYNC_BOOTSTRAP_THREADS, //1025/1026 拉取会话列表中
        SYNC_WAIT_UPSERT,       //等待会话列表写库
        SYNC_WAIT_MARK,         //等待 MarkBootstrapComplete
        SYNC_WAIT_PAGE          //等待 applySyncPage 提交结果
    };
    void sendSyncRequest();
    void sendThreadListRequest(qint64 lastThreadId);
    //envelope JSON → DTO（message_id/thread_id/sync_seq 十进制字符串解析）
    static LocalMessageDTO envelopeToDto(const QJsonObject& envelope);

    SyncState _state;
    qint64 _last_sync_seq;      //内存游标（与 sync_state.last_sync_seq 一致）
    qint64 _checkpoint;         //bootstrap checkpoint
    bool _has_more_pending;     //1052 声明 has_more，等本页提交成功后续拉
    bool _pull_pending;         //拉取中收到 notifyOnlineMessage，完成后补拉一次
    QList<LocalConversationDTO> _bootstrap_convs;
};

#endif // CHATSYNCMANAGER_H
