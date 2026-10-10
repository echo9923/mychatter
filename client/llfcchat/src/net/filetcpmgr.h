#ifndef FILETCPMGR_H
#define FILETCPMGR_H

#include <QTcpSocket>
#include <QTimer>
#include "singleton.h"
#include "global.h"
#include "resourcetransfer.h"
#include "transferengine.h"
#include <functional>
#include <QObject>
#include "userdata.h"
#include <QJsonArray>
#include <memory>
#include <QThread>
#include <QQueue>
#include <atomic>

class FileTcpThread: public std::enable_shared_from_this<FileTcpThread>{
public:
    FileTcpThread();
    ~FileTcpThread();
private:
    QThread * _file_tcp_thread;
};

//ResourceServer 纯传输层（驻 File 线程，P1b-02）。
//
//职责边界：socket 读写、1501 鉴权、断线重连、头像旧通道（1601-1604）。
//资源消息（1505/1507/1509/1511 及其回包）全部委托 TransferEngine：
//引擎经 SetSendCallback 注入的 send 回调发帧，本类把 1506/1508/1510/1512
//原样喂给 OnFrame；不再持有 MsgInfo 传输登记（usermgr 登记表已删除，
//跨线程不共享可变 MsgInfo）。
//
//发送：每帧只调用一次 write()（Qt 内部缓冲保证整帧最终写完，
//bytesWritten 只用于队列轮转，不再重发"剩余部分"）；bytesToWrite()
//超过高水位（默认 4MiB）时入队背压。接收：_buffer.remove 消费，去掉
//每帧 mid 拷贝与逐帧 qDebug 包体。
//
//连接生命周期：1501 由本类在 connected 后自行发送（uid 取缓存
//ServerInfo，token 取 UserMgr）；_manual_close 区分主动断开——主动断开
//不重连，异常断开按退避自动重连，1502 成功后引擎 OnConnectionAuthenticated
//（活跃上传重发 1507、下载从 .part 续传）。只有 1502 返回 2010 才上抛
//sig_resource_token_invalid 让主界面登出，普通断开不再登出。
class FileTcpMgr : public QObject, public Singleton<FileTcpMgr>,
        public std::enable_shared_from_this<FileTcpMgr>
{
    Q_OBJECT
public:
    friend class Singleton<FileTcpMgr>;
    ~FileTcpMgr();

    //---- 头像旧通道（1601/1603，保持不变）----
    void SendData(ReqId reqId, QByteArray data);
    void CloseConnection();
    void SendDownloadInfo(std::shared_ptr<DownloadInfo> download, QString req_type);

    //---- 资源传输（线程安全入口：内部经信号投递到 File 线程再进引擎）----
    void PostStartUpload(const UploadRequest& req);
    void PostStartDownload(const DownloadRequest& req);
    void PostPauseTransfer(int direction, qint64 message_id);
    void PostResumeTransfer(int direction, qint64 message_id);
    void PostCancelTransfer(int direction, qint64 message_id);
    //回包入口（1506/1508/1510/1512；测试与传输层共用）
    void PostFrame(quint16 msg_id, const QByteArray& payload);

    //登录代次只读访问（GUI/TCP 线程 load；写仅在 File 线程 1502 成功/
    //关闭/换代次时递增）：消费端用它过滤旧会话的迟到快照
    int LoginGeneration() const { return _login_generation.load(); }

signals:
    void sig_close();
    void sig_send_data(ReqId reqId, QByteArray data);
    void sig_con_success(bool bsuccess);
    void sig_connection_closed();      //仅 2010 致命场景（配合主界面登出）
    void sig_resource_token_invalid(); //1502 返回 2010：Token 失效须登出
    void sig_reset_label_icon(QString path);
    //引擎快照转发（File 线程 → GUI/outbox queued）
    void sig_transfer_updated(TransferSnapshot snapshot);
    void sig_transfer_finished(TransferSnapshot snapshot);
    //3.2 Resource 登录(1502)成功信号
    void sig_resource_login_success();
    //3.2 Resource 登录(1502)失败信号（携带用户可见错误）
    void sig_resource_login_failed(QString reason);
    //内部跨线程投递（File 线程执行）
    void sig_internal_start_upload(UploadRequest req);
    void sig_internal_start_download(DownloadRequest req);
    void sig_internal_pause(int direction, qint64 message_id);
    void sig_internal_resume(int direction, qint64 message_id);
    void sig_internal_cancel(int direction, qint64 message_id);
    void sig_internal_frame(quint16 msg_id, QByteArray payload);
    void sig_internal_connect(std::shared_ptr<ServerInfo> si);

public slots:
    void slot_send_data(ReqId reqId, QByteArray data);
    void slot_tcp_connect(std::shared_ptr<ServerInfo> si);
    void slot_tcp_close();
    //引擎与鉴权会合（自连接 sig_resource_login_success/failed 驱动，
    //测试也可直接 emit 该组信号）
    void slot_engine_connection_up();
    void slot_engine_connection_down();

private slots:
    void slot_internal_start_upload(UploadRequest req);
    void slot_internal_start_download(DownloadRequest req);
    void slot_internal_pause(int direction, qint64 message_id);
    void slot_internal_resume(int direction, qint64 message_id);
    void slot_internal_cancel(int direction, qint64 message_id);
    void slot_internal_frame(quint16 msg_id, QByteArray payload);
    void slot_reconnect_timeout();

private:
    explicit FileTcpMgr(QObject *parent = nullptr);

    void initHandlers();
    void registerMetaType();
    void handleMsg(ReqId id, int len, QByteArray data);
    //1502 Resource 登录回包（含 2010 判定）
    void handleResourceLoginRsp(ReqId id, int len, QByteArray data);
    //1602 头像上传回包
    void handleUploadHeadIconRsp(ReqId id, int len, QByteArray data);
    //1604 头像/旧文件下载回包
    void handleDownloadFileRsp(ReqId id, int len, QByteArray data);
    //引擎配置：config.ini [Resource] 段（缺省即引擎默认）
    TransferEngine::Config loadEngineConfig();
    //1501：连接建立后自行鉴权（uid 取缓存 ServerInfo，token 取 UserMgr）
    void sendResourceLogin();
    //异常断开：退避自动重连（_manual_close 时不启动）
    void scheduleReconnect();
    //帧出队/背压轮转
    void tryDequeueSend();
    //部分接受后缀补写（write 返回值推进）
    void flushPendingSuffix();
    //复位收发/鉴权/队列/看门狗状态（不触发重连本身）
    void resetConnection();
    //1506/1508/1510/1512 原样转发引擎
    void handleEngineFrameRsp(ReqId id, int len, QByteArray data);

    QTcpSocket _socket;              //成员对象（父子亲缘随 moveToThread 迁移）
    QString _host;
    uint16_t _port;
    QByteArray _buffer;
    bool _b_recv_pending;
    quint16 _message_type;
    quint32 _message_len;
    typedef void (FileTcpMgr::*FileHandler)(ReqId id, int len, QByteArray data);
    QMap<ReqId, FileHandler> _handlers;
    //发送队列：每帧一次 write；高水位背压 + bytesWritten 轮转
    QQueue<QByteArray> _send_queue;
    QByteArray _current_block;
    qint64 _bytes_sent;
    bool _pending;
    //3.2 Resource 鉴权标志：1502 成功前禁止发送任何业务帧
    bool _authenticated;
    //主动关闭标志：主动断开不重连，异常断开按退避重连
    bool _manual_close;
    //重连退避（QTimer 子对象，随本对象线程亲缘）
    QTimer* _reconnect_timer;
    int _reconnect_attempt;
    //缓存最近一次连接参数（重连与 1501 用）
    std::shared_ptr<ServerInfo> _server_info;
    //传输引擎（File 线程独占）
    TransferEngine* _engine;
    qint64 _send_high_watermark;     //bytesToWrite 流水背压高水位
    QTimer* _auth_timer;             //1501 鉴权看门狗（超时按连接死亡重连）
    int _auth_timeout_ms;
    qint64 _send_queue_bytes;        //发送队列字节上界（满则重置连接）
    qint64 _send_queue_limit_bytes;
    qint64 _reconnect_initial_ms;
    //连接时捕获的凭据值快照（1501 只用快照，不跨线程读 UserMgr）
    int _auth_uid;
    QString _auth_token;
    //登录代次：1502 成功/主动关闭/换代次递增（File 线程写，原子）；
    //转发快照时附加，GUI/Outbox 校验同代次后才允许写本地库
    std::atomic<int> _login_generation;
    qint64 _reconnect_max_ms;
};

#endif // FILETCPMGR_H
