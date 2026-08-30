#ifndef FILETCPMGR_H
#define FILETCPMGR_H

#include <QTcpSocket>
#include "singleton.h"
#include "global.h"
#include <functional>
#include <QObject>
#include "userdata.h"
#include <QJsonArray>
#include <memory>
#include <QThread>
#include <QQueue>
#include <memory>
#include "global.h"

class FileTcpThread: public std::enable_shared_from_this<FileTcpThread>{
public:
    FileTcpThread();
    ~FileTcpThread();
private:
    QThread * _file_tcp_thread;

};

//ResourceServer 连接管理（驻 File 线程）。
//头像通道（1601-1604）保持旧 seq+MD5 协议；资源消息通道（1505/1507/1509/1511）
//统一 offset+SHA-256 协议：上传以 message_id 为主键、按服务端 server_offset 对齐，
//下载按 offset 逐片拉取并做片/整文件哈希校验后原子落盘。
class FileTcpMgr : public QObject, public Singleton<FileTcpMgr>,
        public std::enable_shared_from_this<FileTcpMgr>
{
    Q_OBJECT
public:
    friend class Singleton<FileTcpMgr>;
    ~FileTcpMgr();
    void SendData(ReqId reqId, QByteArray data);
    void CloseConnection();
    void SendDownloadInfo(std::shared_ptr<DownloadInfo> download,QString req_type);
    //窗口发送下一批资源分片（1505）：从 MsgInfo 当前确认偏移续读。
    //只能在 File 线程内调用；跨线程请用 PostBatchSend。
    void BatchSend(std::shared_ptr<MsgInfo> msg_info);
    //跨线程入口：投递到 File 线程执行 BatchSend（_cwnd_size 是 File 线程状态）
    void PostBatchSend(std::shared_ptr<MsgInfo> msg_info);
    //启动资源下载：1509 查元数据 -> 1511 逐片下载（自动/手动触发统一入口）
    void StartResourceDownload(std::shared_ptr<MsgInfo> msg_info);
    //暂停/继续/重试既有资源传输（GUI 信号入口）
    void ContinueUploadFile(QString unique_name);
    void ContinueDownloadFile(QString unique_name);
    //复制文件到用户资源目录（发送完成后的本地归档）
    bool CopyFile(QString src_path, QString dst_path, QString dst_dir);
private:
    void initHandlers();
    // 1502 Resource 登录回包
    void handleResourceLoginRsp(ReqId id, int len, QByteArray data);
    // 1602 头像上传回包
    void handleUploadHeadIconRsp(ReqId id, int len, QByteArray data);
    // 1604 头像/旧文件下载回包
    void handleDownloadFileRsp(ReqId id, int len, QByteArray data);
    // 1506 资源分片上传回包
    void handleResourceChunkUploadRsp(ReqId id, int len, QByteArray data);
    // 1508 上传进度查询回包
    void handleResourceUploadProgressRsp(ReqId id, int len, QByteArray data);
    // 1510 资源下载元数据回包
    void handleResourceDownInfoRsp(ReqId id, int len, QByteArray data);
    // 1512 资源分片下载回包
    void handleResourceChunkDownRsp(ReqId id, int len, QByteArray data);
    explicit FileTcpMgr(QObject *parent = nullptr);

    void registerMetaType();
    void handleMsg(ReqId id, int len, QByteArray data);
    //下载缓存目录：AppData/user/<uid>/cache/<message_id>/（按 message_id 隔离）
    QString resourceCacheDir(qint64 message_id);

    QTcpSocket _socket;
    QString _host;
    uint16_t _port;
    QByteArray _buffer;
    bool _b_recv_pending;
    quint16 _message_type;
    quint32 _message_len;
    typedef void (FileTcpMgr::*FileHandler)(ReqId id, int len, QByteArray data);
    QMap<ReqId, FileHandler> _handlers;
    //发送队列
    QQueue<QByteArray> _send_queue;
    //正在发送的包
    QByteArray  _current_block;
    //当前已发送的字节数
    qint64        _bytes_sent;
    //是否正在发送
    bool _pending;
    //3.2 Resource 鉴权标志：1502 成功前禁止发送任何业务帧
    bool _authenticated;
    //发送的拥塞窗口，控制发送数量
    int _cwnd_size;
signals:
    void sig_close();
     void sig_send_data(ReqId reqId, QByteArray data);
     void sig_con_success(bool bsuccess);
     void sig_connection_closed();
     void sig_reset_label_icon(QString path);
     void sig_update_upload_progress(std::shared_ptr<MsgInfo>);
     void sig_start_resource_download(std::shared_ptr<MsgInfo>);
     void sig_continue_upload_file(QString unique_name);
     void sig_continue_download_file(QString unique_name);
     void sig_batch_send(std::shared_ptr<MsgInfo>);
     void sig_update_download_progress(std::shared_ptr<MsgInfo>);
     void sig_download_finish(std::shared_ptr<MsgInfo>,QString file_path);
     //资源上传收全（1506 status=1），OutboxDispatcher 据此 confirmResourceSent
     void sig_resource_upload_done(QString unique_name, QString local_file_path);
     //资源上传永久失败（2115/2116 等），OutboxDispatcher 据此 markSendFailed
     void sig_resource_upload_failed(QString unique_name, int error);
     //资源下载失败/终态（UI 据此把气泡置为失败/已过期）
     void sig_download_failed(std::shared_ptr<MsgInfo>, int error);
     //1507/1508 上传进度查询回包（OutboxDispatcher 重启恢复续传用）
     void sig_upload_progress_rsp(qint64 message_id, int error, qint64 server_offset,
          int status);
     //3.2 Resource 登录(1502)成功信号
     void sig_resource_login_success();
     //3.2 Resource 登录(1502)失败信号（携带用户可见错误）
     void sig_resource_login_failed(QString reason);
public slots:
    void slot_send_data(ReqId reqId, QByteArray data);
    void slot_tcp_connect(std::shared_ptr<ServerInfo> si);
    void slot_tcp_close();
    void slot_start_resource_download(std::shared_ptr<MsgInfo> msg_info);
    void slot_batch_send(std::shared_ptr<MsgInfo> msg_info);
    void slot_continue_upload_file(QString unique_name);
    void slot_continue_download_file(QString unique_name);
};

#endif // FILETCPMGR_H
