#ifndef FILEBUBBLE_H
#define FILEBUBBLE_H

#include "BubbleFrame.h"
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include "global.h"

//文件消息气泡：扩展名图标 + 文件名 + 大小 + 进度条 + 按钮组
//（下载/暂停/继续/打开/另存为）。状态机与 PictureBubble 一致：
//None（待下载）→ Downloading → Paused/Completed/Failed/Expired。
class FileBubble : public BubbleFrame
{
    Q_OBJECT
public:
    FileBubble(const QString& file_name, qint64 total_size, ChatRole role,
        QWidget* parent = nullptr);

    void setProgress(qint64 value, qint64 total_value);
    void setState(TransferState state);
    TransferState state() const { return m_state; }
    void setMsgInfo(std::shared_ptr<MsgInfo> msg);
    //下载完成：切换到完成态并记录本地路径（打开/另存为据此操作）
    void setDownloadFinish(std::shared_ptr<MsgInfo> msg, const QString& file_path);
    //下载失败/过期终态
    void setDownloadFailed(int error);
    QString localPath() const { return m_local_path; }
    QString fileName() const { return m_file_name; }

signals:
    void downloadRequested(QString unique_name);                       // 请求下载（1509）
    void pauseRequested(QString unique_name, TransferType type);       // 请求暂停
    void resumeRequested(QString unique_name, TransferType type);      // 请求继续
    void openRequested(QString file_path);                             // 打开本地文件
    void saveAsRequested(QString src_path, QString file_name);         // 另存为

private slots:
    void onOpenClicked();
    void onSaveAsClicked();

private:
    void refreshButtons();
    static QString humanSize(qint64 bytes);

private:
    QLabel* m_iconLabel;
    QLabel* m_nameLabel;
    QLabel* m_sizeLabel;
    QProgressBar* m_progressBar;
    QPushButton* m_downloadBtn;
    QPushButton* m_pauseBtn;
    QPushButton* m_resumeBtn;
    QPushButton* m_openBtn;
    QPushButton* m_saveAsBtn;
    TransferState m_state;
    QString m_file_name;
    QString m_local_path;
    qint64 m_total_size;
    std::shared_ptr<MsgInfo> _msg_info;
};

#endif // FILEBUBBLE_H
