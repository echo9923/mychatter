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
    //引擎快照驱动：按 message_id 找气泡，推进进度/阶段/终态（方向随快照）
    void ApplyTransferSnapshot(const TransferSnapshot& snapshot);
    //上传终态唯一入口（本地库 commit 信号驱动；网络终态不直接置气泡）
    void SetUploadTerminal(qint64 message_id, bool completed);
    //下载缓存路径（按 message_id 隔离；ChatDialog 共用同一约定）
    static QString resourceCachePath(qint64 message_id, const QString& file_name);

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
    void on_clicked_paused(qint64 message_id, int direction);
    //接收PictureBubble传回来的继续信号
    void on_clicked_resume(qint64 message_id, int direction);
    //本地库入库提交成功后才上屏（sending 气泡）；可靠派发由 Dispatcher
    //订阅同一 sig_send_enqueued 自行登记，与 GUI 互不依赖
    void slot_send_enqueued(bool ok, LocalMessageDTO message,
        LocalMessageResourceDTO resource, OutboxEntryDTO outbox);

private:
    void clearItems();
    Ui::ChatPage *ui;
    std::shared_ptr<ChatThreadData> _chat_data;
    //管理未回复聊天信息
    QHash<QString, ChatItemBase*> _unrsp_item_map;
    //管理已经回复的消息
    QHash<qint64, ChatItemBase*> _base_item_map;
    //GUI 线程自有的展示对象登记（message_id → 气泡 MsgInfo；仅 GUI 线程访问）
    QHash<qint64, std::shared_ptr<MsgInfo>> _msg_infos;
    void RegisterBubbleMsgInfo(qint64 message_id, const std::shared_ptr<MsgInfo>& info);
    //发送中的入库请求（client_message_id -> 消息 / 资源运行时对象）
    QHash<QString, LocalMessageDTO> _pending_sends;
    QHash<QString, std::shared_ptr<MsgInfo>> _pending_img_infos;
};

#endif // CHATPAGE_H
