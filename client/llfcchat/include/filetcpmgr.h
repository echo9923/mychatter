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
    void BatchSend(std::shared_ptr<MsgInfo> msg_info, int sender, int receiver);
    void ContinueUploadFile(QString unique_name);
    void ContinueDownloadFile(QString unique_name);
    void CopyFile(QString src_path, QString dst_path, QString dst_dir);
private:
    void initHandlers();
    explicit FileTcpMgr(QObject *parent = nullptr);

    void registerMetaType();
    void handleMsg(ReqId id, int len, QByteArray data);

    QTcpSocket _socket;
    QString _host;
    uint16_t _port;
    QByteArray _buffer;
    bool _b_recv_pending;
    quint16 _message_type;
    quint32 _message_len;
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handlers;
    //发送队列
    QQueue<QByteArray> _send_queue;
    //正在发送的包
    QByteArray  _current_block;
    //当前已发送的字节数
    qint64        _bytes_sent;
    //是否正在发送
    bool _pending;
    //3.2 Resource 鉴权标志：1054 成功前禁止发送任何业务帧
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
     void sig_continue_upload_file(QString unique_name);
     void sig_continue_download_file(QString unique_name);
     void sig_update_download_progress(std::shared_ptr<MsgInfo>);
     void sig_download_finish(std::shared_ptr<MsgInfo>,QString file_path);
     //聊天图片上传收全（1038/1044 完成分支），OutboxDispatcher 据此 confirmImageSent
     void sig_chat_img_upload_done(QString unique_name);
     //3.2 Resource 登录(1054)成功信号
     void sig_resource_login_success();
     //3.2 Resource 登录(1054)失败信号（携带用户可见错误）
     void sig_resource_login_failed(QString reason);
public slots:
    void slot_send_data(ReqId reqId, QByteArray data);
    void slot_tcp_connect(std::shared_ptr<ServerInfo> si);
    void slot_tcp_close();
    void slot_continue_upload_file(QString unique_name);
    void slot_continue_download_file(QString unique_name);
};

#endif // FILETCPMGR_H
