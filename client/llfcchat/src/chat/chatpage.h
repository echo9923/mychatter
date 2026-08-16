#ifndef CHATPAGE_H
#define CHATPAGE_H

#include <QWidget>
#include "userdata.h"
#include <QMap>
#include <QHash>
#include "chatitembase.h"
#include "localmessageDTO.h"

namespace Ui {
class ChatPage;
}

class ChatPage : public QWidget
{
    Q_OBJECT
public:
    explicit ChatPage(QWidget *parent = nullptr);
    ~ChatPage();
    void SetChatData(std::shared_ptr<ChatThreadData> chat_data);
    void AppendChatMsg(std::shared_ptr<ChatDataBase> msg, bool rsp=true);
    void UpdateChatStatus(std::shared_ptr<ChatDataBase> msg);
    void UpdateImgChatStatus(std::shared_ptr<ImgChatData> img_msg);
    void SetSelfIcon(ChatItemBase* pChatItem, QString icon);
    void UpdateFileProgress(std::shared_ptr<MsgInfo> msg_info);
    void LoadHeadIcon(QString avatarPath, QLabel* icon_label, QString file_name, QString req_type);
    void AppendOtherMsg(std::shared_ptr<ChatDataBase> msg);
    void DownloadFileFinished(std::shared_ptr<MsgInfo> msg_info, QString file_path);
    //下载失败/过期终态：气泡置为失败/已过期
    void DownloadFileFailed(std::shared_ptr<MsgInfo> msg_info);

private:
    //按消息类型创建资源气泡（图片 PictureBubble / 文件 FileBubble）并接好
    //暂停/恢复/下载信号；返回 nullptr 表示类型不支持
    QWidget* makeResourceBubble(ChatMsgType type, const std::shared_ptr<MsgInfo>& info,
        ChatRole role);
protected:
    void paintEvent(QPaintEvent *event);

private slots:
    void on_send_btn_clicked();

    void on_receive_btn_clicked();

    //接收PictureBubble传回来的暂停信号
    void on_clicked_paused(QString unique_name, TransferType transfer_type);
    //接收PictureBubble传回来的继续信号
    void on_clicked_resume(QString unique_name, TransferType transfer_type);
    //本地库入库提交成功后才上屏（sending 气泡）并通知 Dispatcher
    void slot_send_enqueued(bool ok, LocalMessageDTO dto);

private:
    void clearItems();
    Ui::ChatPage *ui;
    std::shared_ptr<ChatThreadData> _chat_data;
    //管理未回复聊天信息
    QHash<QString, ChatItemBase*> _unrsp_item_map;
    //管理已经回复的消息
    QHash<qint64, ChatItemBase*> _base_item_map;
    //发送中的入库请求（client_message_id → DTO / 图片 MsgInfo）
    QHash<QString, LocalMessageDTO> _pending_sends;
    QHash<QString, std::shared_ptr<MsgInfo>> _pending_img_infos;
};

#endif // CHATPAGE_H
