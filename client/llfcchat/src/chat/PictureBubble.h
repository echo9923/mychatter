#ifndef PICTUREBUBBLE_H
#define PICTUREBUBBLE_H

#include "BubbleFrame.h"
#include <QHBoxLayout>
#include <QPixmap>
#include "ClickableLabel.h"
#include <QProgressBar>
#include "global.h"
#include "resourcetransfer.h"
class PictureBubble : public BubbleFrame
{
    Q_OBJECT
public:


    PictureBubble(const QPixmap& picture, ChatRole role,int total, QWidget* parent = nullptr);

    void setProgress(int value, int total_value);
    void showProgress(bool show);
    void setState(TransferState state);
    void resumeState();
    void setMsgInfo(std::shared_ptr<MsgInfo> msg);
    TransferState state() const { return m_state; }
    void setDownloadFinish(std::shared_ptr<MsgInfo> msg, QString file_path);

signals:
    void pauseRequested(qint64 message_id, int direction);              // 请求暂停（引擎意图标志）
    void resumeRequested(qint64 message_id, int direction);             // 请求继续
    void cancelRequested(qint64 message_id, int direction);             // 请求取消

private slots:
    void onPictureClicked();

private:
    void updateIconOverlay();
    void adjustSize();

private:
    ClickableLabel* m_picLabel;
    QProgressBar* m_progressBar;
    TransferState m_state;

    QIcon m_pauseIcon;
    QIcon m_playIcon;
    QIcon m_downloadIcon;
    QSize m_pixmapSize;
    QVBoxLayout* m_vLayout;
    int m_total_size;
    std::shared_ptr<MsgInfo> _msg_info;
};

#endif // PICTUREBUBBLE_H
