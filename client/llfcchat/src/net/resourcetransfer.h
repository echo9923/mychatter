#ifndef RESOURCETRANSFER_H
#define RESOURCETRANSFER_H

#include <QMetaType>
#include <QString>

//P1b 传输引擎值对象（跨线程信号载体）。与 global.h 的 MsgInfo/TransferState
//旧登记表解耦：传输一律以 message_id 为 key，方向显式携带，UI/outbox 只消费
//TransferSnapshot，不再以文件名为 key（同名文件互相覆盖的根因）。

enum TransferDirection {
    TRANSFER_UPLOAD = 0,
    TRANSFER_DOWNLOAD = 1,
};

//引擎对外可见的阶段。Queued=并发槽位未授予；Querying=1507/1509 同步在途；
//Backoff=暂时错误退避；Paused/Completed/Failed 与语义一致。
enum TransferPhase {
    TRANSFER_QUEUED = 0,
    TRANSFER_QUERYING = 1,
    TRANSFER_ACTIVE = 2,
    TRANSFER_BACKOFF = 3,
    TRANSFER_PAUSED = 4,
    TRANSFER_COMPLETED = 5,
    TRANSFER_FAILED = 6,
};

struct UploadRequest {
    qint64 message_id = 0;
    QString file_path;           //源文件绝对路径（发送者本地）
    qint64 file_size_bytes = 0;  //1503 声明的字节数（启动前 size 校验基准）
    QString sha256;              //声明的整文件 SHA-256（小写 hex，2117 复核用）
};

struct DownloadRequest {
    qint64 message_id = 0;
    QString dest_file_path;      //完成后目标路径；"<dest>.part" 为断点缓存
    QString sha256;              //期望整文件 SHA-256（可空，以 1510 为准）
    qint64 file_size_bytes = 0;  //期望大小（0=未知，以 1510 为准）
};

struct TransferSnapshot {
    qint64 message_id = 0;
    int direction = TRANSFER_UPLOAD;
    int phase = TRANSFER_QUEUED;
    qint64 acked_bytes = 0;  //已确认字节（上传=服务端确认；下载=已落盘）
    qint64 sent_bytes = 0;   //已交付传输层字节（发送窗口前缘）
    qint64 total_bytes = 0;
    qint64 event_seq = 0;    //上传发布成功时服务端分配的事件号
    int error_code = 0;      //0=无错/本地错误；否则为协议错误码
    QString error_text;
};

Q_DECLARE_METATYPE(UploadRequest)
Q_DECLARE_METATYPE(DownloadRequest)
Q_DECLARE_METATYPE(TransferSnapshot)

//跨线程 queued 信号使用前调用一次（幂等）。
inline void RegisterResourceTransferMetaTypes()
{
    qRegisterMetaType<UploadRequest>("UploadRequest");
    qRegisterMetaType<DownloadRequest>("DownloadRequest");
    qRegisterMetaType<TransferSnapshot>("TransferSnapshot");
}

#endif // RESOURCETRANSFER_H
