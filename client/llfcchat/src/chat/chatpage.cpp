#include "chatpage.h"
#include "ui_chatpage.h"
#include <QStyleOption>
#include <QPainter>
#include "ChatItemBase.h"
#include "TextBubble.h"
#include "PictureBubble.h"
#include "FileBubble.h"
#include "applyfrienditem.h"
#include "usermgr.h"
#include <QJsonArray>
#include <QJsonObject>
#include "tcpmgr.h"
#include <QUuid>
#include <QStandardPaths>
#include "filetcpmgr.h"
#include "localchatstore.h"
#include "outboxdispatcher.h"
#include <memory>

ChatPage::ChatPage(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ChatPage)
{
    ui->setupUi(this);
    //设置按钮样式
    ui->receive_btn->SetState("normal","hover","press");
    ui->send_btn->SetState("normal","hover","press");

    //设置图标样式
    ui->emo_lb->SetState("normal","hover","press","normal","hover","press");
    ui->file_lb->SetState("normal","hover","press","normal","hover","press");

    //入库提交成功后才上屏（sending 气泡）
    connect(LocalChatStore::GetInstance().get(), &LocalChatStore::sig_send_enqueued,
        this, &ChatPage::slot_send_enqueued);

}

ChatPage::~ChatPage()
{
    delete ui;
}

void ChatPage::SetChatData(std::shared_ptr<ChatThreadData> chat_data) {
    _chat_data = chat_data;
    auto other_id = _chat_data->GetOtherId();
    if(other_id == 0) {
        //说明是群聊
        ui->title_lb->setText(_chat_data->GetGroupName());
        //todo...加载群聊信息和成员信息
        return;
    }

    //私聊
    auto friend_info = UserMgr::GetInstance()->GetFriendById(other_id);
    if (friend_info == nullptr) {
        return;
    }
    ui->title_lb->setText(friend_info->_name);
    ui->chat_data_list->removeAllItem();
    _unrsp_item_map.clear();
    _base_item_map.clear();
    for(auto & msg : chat_data->GetMsgMapRef()){
        AppendChatMsg(msg);
    }

    for (auto& msg : chat_data->GetMsgUnRspRef()) {
        AppendChatMsg(msg,false);
    }
}

void ChatPage::AppendChatMsg(std::shared_ptr<ChatDataBase> msg, bool rsp)
{
    auto self_info = UserMgr::GetInstance()->GetUserInfo();
    ChatRole role;
    if (msg->GetSendUid() == self_info->_uid) {
        role = ChatRole::Self;
        ChatItemBase* pChatItem = new ChatItemBase(role);
        
        pChatItem->setUserName(self_info->_name);
        SetSelfIcon(pChatItem, self_info->_icon);
        QWidget* pBubble = nullptr;
        if (msg->GetMsgType() == ChatMsgType::TEXT) {
            pBubble = new TextBubble(role, msg->GetMsgContent());
        }else if (msg->GetMsgType() == ChatMsgType::PIC || msg->GetMsgType() == ChatMsgType::FILE) {
            pBubble = makeResourceBubble(msg->GetMsgType(),
                dynamic_pointer_cast<ImgChatData>(msg)->_msg_info, role);
        }
     
        pChatItem->setWidget(pBubble);
        auto status = msg->GetStatus();
        pChatItem->setStatus(status);
        ui->chat_data_list->appendChatItem(pChatItem);
        if (rsp) {
            _base_item_map[msg->GetMsgId()] = pChatItem;
        }
        else {
            _unrsp_item_map[msg->GetUniqueId()] = pChatItem;
        }
       
    }
    else {
        role = ChatRole::Other;
        ChatItemBase* pChatItem = new ChatItemBase(role);
        auto friend_info = UserMgr::GetInstance()->GetFriendById(msg->GetSendUid());
        if (friend_info == nullptr) {
            return;
        }
    
        pChatItem->setUserName(friend_info->_name);
        
        // 使用正则表达式检查是否是默认头像
        QRegularExpression regex("^:/res/head_(\\d+)\\.jpg$");
        QRegularExpressionMatch match = regex.match(friend_info->_icon);
        if (match.hasMatch()) {
            pChatItem->setUserIcon(QPixmap(friend_info->_icon));
        }
        else {
            // 如果是用户上传的头像，获取存储目录
            QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            auto uid = UserMgr::GetInstance()->GetUid();
            QDir avatarsDir(storageDir + "/user/" + QString::number(msg->GetSendUid()) + "/avatars");
            // 确保目录存在
            if (avatarsDir.exists()) {
                QString avatarPath = avatarsDir.filePath(friend_info->_icon); // 获取上传头像的完整路径
                QPixmap pixmap(avatarPath); // 加载上传的头像图片
                if (!pixmap.isNull()) {
                    pChatItem->setUserIcon(pixmap);
                }
                else {
                    qWarning() << "无法加载上传的头像：" << avatarPath;
                    auto icon_label = pChatItem->getIconLabel();
                    LoadHeadIcon(avatarPath, icon_label, friend_info->_icon,"other_icon");
                }
            }
            else {
                qWarning() << "头像存储目录不存在：" << avatarsDir.path();
                //创建目录
                avatarsDir.mkpath(".");         
                auto icon_label = pChatItem->getIconLabel();
                QString avatarPath = avatarsDir.filePath(friend_info->_icon);
                LoadHeadIcon(avatarPath, icon_label, friend_info->_icon, "other_icon");
            }
        }

        QWidget* pBubble = nullptr;
        if (msg->GetMsgType() == ChatMsgType::TEXT) {
            pBubble = new TextBubble(role, msg->GetMsgContent());
        }
        else if (msg->GetMsgType() == ChatMsgType::PIC || msg->GetMsgType() == ChatMsgType::FILE) {
            pBubble = makeResourceBubble(msg->GetMsgType(),
                dynamic_pointer_cast<ImgChatData>(msg)->_msg_info, role);
        }
        pChatItem->setWidget(pBubble);
        auto status = msg->GetStatus();
        pChatItem->setStatus(status);
        ui->chat_data_list->appendChatItem(pChatItem);
        if (rsp) {
            _base_item_map[msg->GetMsgId()] = pChatItem;
        }
        else {
            _unrsp_item_map[msg->GetUniqueId()] = pChatItem;
        }
    }

}

void ChatPage::AppendOtherMsg(std::shared_ptr<ChatDataBase> msg) {
    auto self_info = UserMgr::GetInstance()->GetUserInfo();
    ChatRole role;
    if (msg->GetSendUid() == self_info->_uid) {
        role = ChatRole::Self;
        ChatItemBase* pChatItem = new ChatItemBase(role);

        pChatItem->setUserName(self_info->_name);
        SetSelfIcon(pChatItem, self_info->_icon);
        QWidget* pBubble = nullptr;
        if (msg->GetMsgType() == ChatMsgType::TEXT) {
            pBubble = new TextBubble(role, msg->GetMsgContent());
        }
        else if (msg->GetMsgType() == ChatMsgType::PIC || msg->GetMsgType() == ChatMsgType::FILE) {
            pBubble = makeResourceBubble(msg->GetMsgType(),
                dynamic_pointer_cast<ImgChatData>(msg)->_msg_info, role);
        }

        pChatItem->setWidget(pBubble);
        auto status = msg->GetStatus();
        pChatItem->setStatus(status);
        ui->chat_data_list->appendChatItem(pChatItem);
        _base_item_map[msg->GetMsgId()] = pChatItem;
    }
    else {
        role = ChatRole::Other;
        ChatItemBase* pChatItem = new ChatItemBase(role);
        auto friend_info = UserMgr::GetInstance()->GetFriendById(msg->GetSendUid());
        if (friend_info == nullptr) {
            return;
        }
        pChatItem->setUserName(friend_info->_name);

        // 使用正则表达式检查是否是默认头像
        QRegularExpression regex("^:/res/head_(\\d+)\\.jpg$");
        QRegularExpressionMatch match = regex.match(friend_info->_icon);
        if (match.hasMatch()) {
            pChatItem->setUserIcon(QPixmap(friend_info->_icon));
        }
        else {
            // 如果是用户上传的头像，获取存储目录
            QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            auto uid = UserMgr::GetInstance()->GetUid();
            QDir avatarsDir(storageDir + "/user/" + QString::number(uid) + "/avatars");
            auto file_name = QFileInfo(self_info->_icon).fileName();
            // 确保目录存在
            if (avatarsDir.exists()) {
                QString avatarPath = avatarsDir.filePath(file_name); // 获取上传头像的完整路径
                QPixmap pixmap(avatarPath); // 加载上传的头像图片
                if (!pixmap.isNull()) {
                    pChatItem->setUserIcon(pixmap);
                }
                else {
                    qWarning() << "无法加载上传的头像：" << avatarPath;
                    auto icon_label = pChatItem->getIconLabel();
                    LoadHeadIcon(avatarPath, icon_label, file_name, "self_icon");
                }
            }
            else {
                qWarning() << "头像存储目录不存在：" << avatarsDir.path();
                //创建目录
                avatarsDir.mkpath(".");
                auto icon_label = pChatItem->getIconLabel();
                QString avatarPath = avatarsDir.filePath(file_name);
                LoadHeadIcon(avatarPath, icon_label, file_name, "self_icon");
            }
        }

        QWidget* pBubble = nullptr;
        if (msg->GetMsgType() == ChatMsgType::TEXT) {
            pBubble = new TextBubble(role, msg->GetMsgContent());
        }
        else if (msg->GetMsgType() == ChatMsgType::PIC || msg->GetMsgType() == ChatMsgType::FILE) {
            pBubble = makeResourceBubble(msg->GetMsgType(),
                dynamic_pointer_cast<ImgChatData>(msg)->_msg_info, role);
        }
        pChatItem->setWidget(pBubble);
        auto status = msg->GetStatus();
        pChatItem->setStatus(status);
        ui->chat_data_list->appendChatItem(pChatItem);
        _base_item_map[msg->GetMsgId()] = pChatItem;
    }
}

void ChatPage::LoadHeadIcon(QString avatarPath, QLabel* icon_label, QString file_name, QString req_type) {
    UserMgr::GetInstance()->AddLabelToReset(avatarPath, icon_label);
    //先加载默认的
    QPixmap pixmap(":/res/head_1.jpg");
    QPixmap scaledPixmap = pixmap.scaled(icon_label->size(),
        Qt::KeepAspectRatio, Qt::SmoothTransformation); // 将图片缩放到label的大小
    icon_label->setPixmap(scaledPixmap); // 将缩放后的图片设置到QLabel上
    icon_label->setScaledContents(true); // 设置QLabel自动缩放图片内容以适应大小

    //判断是否正在下载
    bool is_loading = UserMgr::GetInstance()->IsDownLoading(file_name);
    if (is_loading) {
        qWarning() << "正在下载: " << file_name;
    }
    else {
        //发送请求获取资源
        auto download_info = std::make_shared<DownloadInfo>();
        download_info->_name = file_name;
        download_info->_current_size = 0;
        download_info->_seq = 1;
        download_info->_total_size = 0;
        download_info->_client_path = avatarPath;
        //添加文件到管理者
        UserMgr::GetInstance()->AddDownloadFile(file_name, download_info);
        //发送消息
        FileTcpMgr::GetInstance()->SendDownloadInfo(download_info, req_type);
    }
}

void ChatPage::UpdateChatStatus(std::shared_ptr<ChatDataBase> msg)
{
    auto iter = _unrsp_item_map.find(msg->GetUniqueId());
    //没找到则直接返回
    if (iter == _unrsp_item_map.end()) {
        return;
    }

    iter.value()->setStatus(msg->GetStatus());
    _base_item_map[msg->GetMsgId()] = iter.value();
    _unrsp_item_map.erase(iter);
}

void ChatPage::UpdateImgChatStatus(std::shared_ptr<ImgChatData> msg) {
    auto iter = _unrsp_item_map.find(msg->GetUniqueId());
    //没找到则直接返回
    if (iter == _unrsp_item_map.end()) {
        return;
    }

    iter.value()->setStatus(msg->GetStatus());
    _base_item_map[msg->GetMsgId()] = iter.value();
    _unrsp_item_map.erase(iter);

    auto bubble = _base_item_map[msg->GetMsgId()]->getBubble();
    //图片用 PictureBubble，文件用 FileBubble，都要回填 MsgInfo
    if (auto* pic_bubble = dynamic_cast<PictureBubble*>(bubble)) {
        pic_bubble->setMsgInfo(msg->_msg_info);
    }
    else if (auto* file_bubble = dynamic_cast<FileBubble*>(bubble)) {
        file_bubble->setMsgInfo(msg->_msg_info);
    }
}

void ChatPage::UpdateFileProgress(std::shared_ptr<MsgInfo> msg_info) {
    auto iter = _base_item_map.find(msg_info->_msg_id);
    if (iter == _base_item_map.end()) {
        return;
    }

    if (msg_info->_msg_type == MsgType::IMG_MSG) {
        auto bubble = iter.value()->getBubble();
        PictureBubble*  pic_bubble = dynamic_cast<PictureBubble*>(bubble);
        pic_bubble->setProgress(msg_info->_rsp_size, msg_info->_total_size);
    }
    else if (msg_info->_msg_type == MsgType::FILE_MSG) {
        auto bubble = iter.value()->getBubble();
        FileBubble* file_bubble = dynamic_cast<FileBubble*>(bubble);
        if (file_bubble) {
            //状态跟随传输方向：上传中/下载中，不强制覆盖（暂停/完成由各自信号设置）
            if (msg_info->_transfer_state == TransferState::Uploading
                || msg_info->_transfer_state == TransferState::Downloading) {
                file_bubble->setState(msg_info->_transfer_state);
            }
            file_bubble->setProgress(msg_info->_rsp_size, msg_info->_total_size);
        }
    }
}

void ChatPage::DownloadFileFinished(std::shared_ptr<MsgInfo> msg_info, QString file_path) {
    auto iter = _base_item_map.find(msg_info->_msg_id);
    if (iter == _base_item_map.end()) {
        return;
    }

    if (msg_info->_msg_type == MsgType::IMG_MSG) {
        auto bubble = iter.value()->getBubble();
        PictureBubble* pic_bubble = dynamic_cast<PictureBubble*>(bubble);
        pic_bubble->setDownloadFinish(msg_info, file_path);
        auto chat_data_base = _chat_data->GetChatDataBase(msg_info->_msg_id);
        if (chat_data_base == nullptr) {
            return;
        }
        auto img_data = dynamic_pointer_cast<ImgChatData>(chat_data_base);
        img_data->_msg_info->_preview_pix =  QPixmap(file_path);
        img_data->_msg_info->_transfer_state = TransferState::Completed;
        img_data->_msg_info->_current_size = img_data->_msg_info->_total_size;
    }
    else if (msg_info->_msg_type == MsgType::FILE_MSG) {
        auto bubble = iter.value()->getBubble();
        FileBubble* file_bubble = dynamic_cast<FileBubble*>(bubble);
        if (file_bubble) {
            file_bubble->setDownloadFinish(msg_info, file_path);
        }
        auto chat_data_base = _chat_data->GetChatDataBase(msg_info->_msg_id);
        if (chat_data_base == nullptr) {
            return;
        }
        auto file_data = dynamic_pointer_cast<ImgChatData>(chat_data_base);
        if (file_data && file_data->_msg_info) {
            file_data->_msg_info->_local_download_path = file_path;
            file_data->_msg_info->_transfer_state = TransferState::Completed;
            file_data->_msg_info->_current_size = file_data->_msg_info->_total_size;
        }
    }
}

QWidget* ChatPage::makeResourceBubble(ChatMsgType type, const std::shared_ptr<MsgInfo>& info,
    ChatRole role)
{
    if (!info) {
        return nullptr;
    }
    if (type == ChatMsgType::PIC) {
        auto pic_bubble = new PictureBubble(info->_preview_pix, role, info->_total_size);
        pic_bubble->setMsgInfo(info);
        connect(pic_bubble, &PictureBubble::pauseRequested,
            this, &ChatPage::on_clicked_paused);
        connect(pic_bubble, &PictureBubble::resumeRequested,
            this, &ChatPage::on_clicked_resume);
        return pic_bubble;
    }
    if (type == ChatMsgType::FILE) {
        auto file_bubble = new FileBubble(info->_unique_name, info->_total_size, role);
        file_bubble->setMsgInfo(info);
        file_bubble->setState(info->_transfer_state);
        //接收方向：下载按钮触发 1511；暂停/继续复用既有链路
        connect(file_bubble, &FileBubble::downloadRequested, this,
            [this](QString unique_name) {
                auto info = UserMgr::GetInstance()->GetTransFileByName(unique_name);
                if (info && info->_msg_id > 0) {
                    FileTcpMgr::GetInstance()->StartResourceDownload(info);
                }
            });
        connect(file_bubble, &FileBubble::pauseRequested,
            this, &ChatPage::on_clicked_paused);
        connect(file_bubble, &FileBubble::resumeRequested,
            this, &ChatPage::on_clicked_resume);
        return file_bubble;
    }
    return nullptr;
}

void ChatPage::DownloadFileFailed(std::shared_ptr<MsgInfo> msg_info) {
    auto iter = _base_item_map.find(msg_info->_msg_id);
    if (iter == _base_item_map.end()) {
        return;
    }

    if (msg_info->_msg_type == MsgType::FILE_MSG) {
        auto bubble = iter.value()->getBubble();
        FileBubble* file_bubble = dynamic_cast<FileBubble*>(bubble);
        if (file_bubble) {
            file_bubble->setState(msg_info->_transfer_state == TransferState::Expired
                ? TransferState::Expired : TransferState::Failed);
        }
    }
}

void ChatPage::SetSelfIcon(ChatItemBase* pChatItem, QString icon)
{
    // 使用正则表达式检查是否是默认头像
    QRegularExpression regex("^:/res/head_(\\d+)\\.jpg$");
    QRegularExpressionMatch match = regex.match(icon);
    if (match.hasMatch()) {
        pChatItem->setUserIcon(QPixmap(icon));
    }
    else {
        // 如果是用户上传的头像，获取存储目录
        QString storageDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        auto uid = UserMgr::GetInstance()->GetUid();
        QDir avatarsDir(storageDir + "/user/" + QString::number(uid) + "/avatars");
        auto file_name = QFileInfo(icon).fileName();
        // 确保目录存在
        if (avatarsDir.exists()) {
            QString avatarPath = avatarsDir.filePath(file_name); // 获取上传头像的完整路径
            QPixmap pixmap(avatarPath); // 加载上传的头像图片
            if (!pixmap.isNull()) {
                pChatItem->setUserIcon(pixmap);
            }
            else {
                qWarning() << "无法加载上传的头像：" << avatarPath;
                auto icon_label = pChatItem->getIconLabel();
                LoadHeadIcon(avatarPath, icon_label, file_name, "self_icon");
            }
        }
        else {
            qWarning() << "头像存储目录不存在：" << avatarsDir.path();
            //创建目录
            avatarsDir.mkpath(".");
            auto icon_label = pChatItem->getIconLabel();
            QString avatarPath = avatarsDir.filePath(file_name);
            LoadHeadIcon(avatarPath, icon_label, file_name, "self_icon");
        }
    }
}

void ChatPage::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QStyleOption opt;
    opt.init(this);
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void ChatPage::on_send_btn_clicked() {
    if (_chat_data == nullptr) {
        qDebug() << "friend_info is empty";
        return;
    }

    auto user_info = UserMgr::GetInstance()->GetUserInfo();
    auto pTextEdit = ui->chatEdit;

    const QVector<std::shared_ptr<MsgInfo>>& msgList = pTextEdit->getMsgList();
    auto thread_id = _chat_data->GetThreadId();
    for (int i = 0; i < msgList.size(); ++i)
    {
        //消息内容长度不合规就跳过
        if (msgList[i]->_text_or_url.length() > 1024) {
            continue;
        }

        MsgType type = msgList[i]->_msg_type;
        //生成唯一id
        QUuid uuid = QUuid::createUuid();
        //转为字符串
        QString uuidString = uuid.toString();

        //文本与资源（图片/文件）都先 enqueueSend 入库，提交成功信号回来才上屏并通知 Dispatcher
        LocalMessageDTO dto;
        dto.client_message_id = uuidString;
        dto.thread_id = thread_id;
        dto.sender_id = user_info->_uid;
        dto.receiver_id = _chat_data->GetOtherId();
        if (type == MsgType::TEXT_MSG)
        {
            QByteArray utf8Message = msgList[i]->_text_or_url.toUtf8();
            dto.message_type = static_cast<int>(ChatMsgType::TEXT);
            dto.content = QString::fromUtf8(utf8Message);
            dto.content_size = "0";
        }
        else if (type == MsgType::IMG_MSG || type == MsgType::FILE_MSG)
        {
            dto.message_type = static_cast<int>(type == MsgType::IMG_MSG
                ? ChatMsgType::PIC : ChatMsgType::FILE);
            //content 为原始文件名（仅展示；服务端磁盘以 message_id 命名），
            //local_path 为本地源文件路径（重传/续传依据）
            dto.content = msgList[i]->_unique_name;
            dto.local_path = msgList[i]->_text_or_url;
            dto.content_size = QString::number(msgList[i]->_total_size);
            dto.resource_status = RESOURCE_UPLOADING;
            //哈希在 MessageTextEdit 采集时已算好（整文件 + 分片），未算则此处补算
            if (msgList[i]->_content_hash.isEmpty()) {
                QString whole;
                QVector<QString> chunks;
                if (!calculateFileSha256(msgList[i]->_text_or_url, whole, chunks)) {
                    qWarning() << "[ChatPage] hash source missing, skip:" << msgList[i]->_text_or_url;
                    continue;
                }
                msgList[i]->_content_hash = whole;
                msgList[i]->_chunk_hashes = chunks;
            }
            dto.content_hash = msgList[i]->_content_hash;
            dto.mime_type = guessMimeType(msgList[i]->_unique_name);
            _pending_img_infos[uuidString] = msgList[i];
        }
        _pending_sends[uuidString] = dto;
        LocalChatStore::GetInstance()->enqueueSend(dto);
    }
}

//入库提交成功：上屏（sending 气泡）+ 通知 Dispatcher 立即派发
void ChatPage::slot_send_enqueued(bool ok, LocalMessageDTO dto)
{
    auto iter = _pending_sends.find(dto.client_message_id);
    if (iter == _pending_sends.end()) {
        return;
    }
    _pending_sends.erase(iter);
    if (!ok) {
        qWarning() << "[ChatPage] enqueue send failed:" << dto.client_message_id;
        _pending_img_infos.remove(dto.client_message_id);
        return;
    }

    auto user_info = UserMgr::GetInstance()->GetUserInfo();
    ChatRole role = ChatRole::Self;
    ChatItemBase* pChatItem = new ChatItemBase(role);
    pChatItem->setUserName(user_info->_name);
    SetSelfIcon(pChatItem, user_info->_icon);
    QWidget* pBubble = nullptr;

    auto thread_data = UserMgr::GetInstance()->GetChatThreadByThreadId(dto.thread_id);
    if (dto.message_type == static_cast<int>(ChatMsgType::TEXT)) {
        pBubble = new TextBubble(role, dto.content);
        //注意，此处先按私聊处理
        auto txt_msg = std::make_shared<TextChatData>(dto.client_message_id, dto.thread_id,
            ChatFormType::PRIVATE, ChatMsgType::TEXT, dto.content, user_info->_uid, 0);
        //将未回复的消息加入到未回复列表中，以便后续处理
        if (thread_data) {
            thread_data->AppendUnRspMsg(dto.client_message_id, txt_msg);
        }
    }
    else if (dto.message_type == static_cast<int>(ChatMsgType::PIC)
        || dto.message_type == static_cast<int>(ChatMsgType::FILE)) {
        auto file_info = _pending_img_infos.take(dto.client_message_id);
        if (!file_info) {
            delete pChatItem;
            return;
        }
        if (dto.message_type == static_cast<int>(ChatMsgType::PIC)) {
            auto pic_bubble = new PictureBubble(QPixmap(dto.local_path), role, file_info->_total_size);
            pic_bubble->setMsgInfo(file_info);
            pBubble = pic_bubble;
            //链接暂停/恢复信号
            connect(dynamic_cast<PictureBubble*>(pBubble), &PictureBubble::pauseRequested,
                this, &ChatPage::on_clicked_paused);
            connect(dynamic_cast<PictureBubble*>(pBubble), &PictureBubble::resumeRequested,
                this, &ChatPage::on_clicked_resume);
        } else {
            //发送方向：标记为上传，FileBubble 据此显示暂停/继续而非下载
            file_info->_transfer_type = TransferType::Upload;
            file_info->_transfer_state = TransferState::Uploading;
            auto file_bubble = new FileBubble(dto.content, file_info->_total_size, role);
            file_bubble->setMsgInfo(file_info);
            pBubble = file_bubble;
            //发送方向的文件：暂停/恢复上传
            connect(file_bubble, &FileBubble::pauseRequested,
                this, &ChatPage::on_clicked_paused);
            connect(file_bubble, &FileBubble::resumeRequested,
                this, &ChatPage::on_clicked_resume);
        }
        auto img_msg = std::make_shared<ImgChatData>(file_info, dto.client_message_id,
            dto.thread_id, ChatFormType::PRIVATE,
            static_cast<ChatMsgType>(dto.message_type), user_info->_uid, 0);
        //将未回复的消息加入到未回复列表中，以便后续处理
        if (thread_data) {
            thread_data->AppendUnRspMsg(dto.client_message_id, img_msg);
        }
        //文件信息加入管理（1504 后 Dispatcher 复用同一 MsgInfo 启动上传）
        UserMgr::GetInstance()->AddTransFile(dto.content, file_info);
    }

    //发送消息上屏（仅当前打开的会话）
    if (pBubble != nullptr) {
        pChatItem->setWidget(pBubble);
        pChatItem->setStatus(0);
        if (_chat_data && _chat_data->GetThreadId() == dto.thread_id) {
            ui->chat_data_list->appendChatItem(pChatItem);
            _unrsp_item_map[dto.client_message_id] = pChatItem;
        }
        else {
            delete pChatItem;
        }
    }

    //通知 Dispatcher 立即派发 outbox 条目
    OutboxDispatcher::GetInstance()->notifySendEnqueued(dto);
}


void ChatPage::on_receive_btn_clicked()
{
    auto pTextEdit = ui->chatEdit;
    ChatRole role = ChatRole::Other;
    auto friend_info = UserMgr::GetInstance()->GetFriendById(_chat_data->GetOtherId());
    QString userName = friend_info->_name;
    QString userIcon = friend_info->_icon;

    const QVector<std::shared_ptr<MsgInfo>>& msgList = pTextEdit->getMsgList();
    for(int i=0; i<msgList.size(); ++i)
    {
        MsgType type = msgList[i]->_msg_type;
        ChatItemBase *pChatItem = new ChatItemBase(role);
        pChatItem->setUserName(userName);
        pChatItem->setUserIcon(QPixmap(userIcon));
        QWidget *pBubble = nullptr;
        if(type == MsgType::TEXT_MSG)
        {
            pBubble = new TextBubble(role, msgList[i]->_text_or_url);
        }
        else if(type == MsgType::IMG_MSG || type == MsgType::FILE_MSG)
        {
            pBubble = makeResourceBubble(
                type == MsgType::IMG_MSG ? ChatMsgType::PIC : ChatMsgType::FILE,
                msgList[i], role);
        }
        if(pBubble != nullptr)
        {
            pChatItem->setWidget(pBubble);
            pChatItem->setStatus(2);
            ui->chat_data_list->appendChatItem(pChatItem);
        }
    }
}

void ChatPage::on_clicked_paused(QString unique_name, TransferType transfer_type)
{
    UserMgr::GetInstance()->PauseTransFileByName(unique_name);
}

void ChatPage::on_clicked_resume(QString unique_name, TransferType transfer_type)
{
    UserMgr::GetInstance()->ResumeTransFileByName(unique_name);
    //继续发送或者下载
    if (transfer_type == TransferType::Upload) {
        FileTcpMgr::GetInstance()->ContinueUploadFile(unique_name);
        return;
    }

    if (transfer_type == TransferType::Download) {
        FileTcpMgr::GetInstance()->ContinueDownloadFile(unique_name);
        return;
    }
}

void ChatPage::clearItems()
{
    ui->chat_data_list->removeAllItem();
    _unrsp_item_map.clear();
    _base_item_map.clear();
}
