#ifndef CHATDIALOG_H
#define CHATDIALOG_H

#include <QDialog>
#include <QLabel>
#include <QList>
#include "statelabel.h"
#include "global.h"
#include "statewidget.h"
#include <memory>
#include "userdata.h"
#include <QListWidgetItem>
#include "loadingdlg.h"
#include "tcpmgr.h"
#include "localchatstore.h"
namespace Ui {
class ChatDialog;
}

class ChatDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatDialog(QWidget *parent = nullptr);
    ~ChatDialog();
    //秒开：从本地 SQLite 加载会话列表，后台由 ChatSyncManager 增量刷新
    void loadChatList();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override ;

    void handleGlobalMousePress(QMouseEvent *event) ;
    void LoadHeadIcon(QString avatarPath, QLabel* icon_label, QString file_name, QString req_type);
private:
    void showLoadingDlg(bool show = true);
    void AddLBGroup(StateWidget* lb);
    void ClearLabelState(StateWidget* lb);
    void loadMoreConUser();
    void SetSelectChatItem(qint64 thread_id = 0);
    void SetSelectChatPage(qint64 thread_id = 0);
    //§6.4 抽取自 slot_create_private_chat：为 thread 不存在时创建 ChatThreadData + 列表项
    QListWidgetItem* createPrivateChatItem(int other_id, qint64 thread_id);
    //本地消息 DTO → 窗口消息对象（文本/图片）
    std::shared_ptr<ChatDataBase> buildChatData(const LocalMessageDTO& dto);
    //只对实际插入的消息上屏（insertedIds 去重保证推送与同步只展示一次）
    void displayInsertedMessages(const QList<LocalMessageDTO>& msgs,
        const QList<qint64>& insertedIds);
    //本地不足时发 1029 拉更早历史（before_message_id 十进制字符串）
    void requestOlderHistory(qint64 thread_id, qint64 oldest_loaded);
    Ui::ChatDialog *ui;
    bool _b_loading;
    QList<StateWidget*> _lb_list;
    void ShowSearch(bool bsearch = false);
    ChatUIMode _mode;
    ChatUIMode _state;
    QWidget* _last_widget;
    //chat_thred_id和对应的item的映射关系。
    QMap<qint64, QListWidgetItem*>  _chat_thread_items;
    qint64 _cur_chat_thread_id;
    QTimer * _timer;
    LoadingDlg* _loading_dlg;

public slots:
    void slot_side_chat();
    void slot_side_contact();
    void slot_side_setting();
    void slot_text_changed(const QString & str);
    void slot_loading_contact_user();
    void slot_switch_apply_friend_page();
    void slot_friend_info_page(std::shared_ptr<UserInfo> user_info);
    void slot_show_search(bool show);
    void slot_apply_friend(std::shared_ptr<AddFriendApply> apply);
    void slot_add_auth_friend(std::shared_ptr<AuthInfo> auth_info);
    void slot_auth_rsp(std::shared_ptr<AuthRsp> auth_rsp);
    void slot_jump_chat_item(std::shared_ptr<SearchInfo> si);
    void slot_jump_chat_item_from_infopage(std::shared_ptr<UserInfo> ui);
    void slot_item_clicked(QListWidgetItem *item);
    void slot_text_chat_msg(std::shared_ptr<TextChatData> msg);
    void slot_img_chat_msg(std::shared_ptr<ImgChatData> imgchat);
    void slot_create_private_chat(int uid, int other_id, qint64 thread_id);

    void slot_load_chat_msg(qint64 thread_id, qint64 msg_id, bool load_more,
        std::vector<std::shared_ptr<ChatDataBase>> msglists);

    //—— 本地库结果信号（提交成功后才更新 UI）——
    void slot_conversations_loaded(bool ok, QList<LocalConversationDTO> convs);
    void slot_recent_messages_loaded(bool ok, qint64 threadId, QList<LocalMessageDTO> msgs,
        bool historyComplete, qint64 oldestLoadedMessageId);
    void slot_history_page_inserted(bool ok, qint64 threadId);
    void slot_incoming_inserted(bool ok, QList<LocalMessageDTO> msgs, QList<qint64> insertedIds);
    void slot_sync_page_applied(bool ok, qint64 newSyncSeq, QList<LocalMessageDTO> msgs,
        QList<qint64> insertedIds);
    void slot_send_confirmed(bool ok, LocalMessageDTO dto);
    void slot_send_failed_marked(bool ok, LocalMessageDTO dto);
    void slot_resource_stage_updated(bool ok, LocalMessageDTO dto);
    void slot_reset_icon(QString path);
    void slot_update_upload_progress(std::shared_ptr<MsgInfo> msg_info);
    void slot_update_download_progress(std::shared_ptr<MsgInfo> msg_info);
    void slot_download_finish(std::shared_ptr<MsgInfo> msg_info, QString file_path);
    void slot_download_failed(std::shared_ptr<MsgInfo> msg_info, int error);
private slots:
    void slot_reset_head();
};



#endif // CHATDIALOG_H
