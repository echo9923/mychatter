#ifndef GLOBAL_H
#define GLOBAL_H
#include <QWidget>
#include <functional>
#include "QStyle"
#include <memory>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QDir>
#include <QSettings>
#include <QVector>
#include <set>
#include <queue>
#include "protocol_ids.h"

//TCP文件上传包头长度
#define FILE_UPLOAD_HEAD_LEN 6
//TCP ID长度
#define FILE_UPLOAD_ID_LEN 2
//TCP 长度字段的长度
#define FILE_UPLOAD_LEN_LEN 4
//最大文件长度
#define MAX_FILE_LEN (1024*32)
//定义最大拥塞窗口的大小
#define MAX_CWND_SIZE 5


/**
 * @brief repolish用来根据属性刷新qss
 */
extern std::function<void(QWidget*)> repolish;
extern std::function<QString(QString)> xorString;

/**
 * @brief The ReqId enum 表示请求的id
 *
 * 数值唯一来源为 proto/protocol_ids.h（llfc_proto 命名空间）。
 * 编号规则：百位=功能域（10账户/11连接/12好友/13聊天/14同步/15资源/16头像），
 * TCP 业务奇数=发起方（请求或服务端通知），偶数=回包；请求/响应连续成对编号。
 * 1001~1005 仅为 HTTP 回调路由 ID，不适用该配对规则。
 */
enum ReqId{
    ID_GET_VARIFY_CODE = llfc_proto::MSG_GET_VARIFY_CODE,     //1001 获取验证码
    ID_REG_USER = llfc_proto::MSG_REG_USER,                   //1002 注册用户
    ID_RESET_PWD = llfc_proto::MSG_RESET_PWD,                 //1003 重置密码
    ID_LOGIN_USER = llfc_proto::MSG_LOGIN_USER,               //1004 用户登录
    ID_CHAT_LOGIN = llfc_proto::MSG_CHAT_LOGIN,               //1101 登陆聊天服务器
    ID_CHAT_LOGIN_RSP= llfc_proto::MSG_CHAT_LOGIN_RSP,        //1102 登陆聊天服务器回包
    ID_HEART_BEAT_REQ = llfc_proto::MSG_HEART_BEAT_REQ,       //1103 心跳请求
    ID_HEARTBEAT_RSP = llfc_proto::MSG_HEARTBEAT_RSP,         //1104 心跳回复
    ID_NOTIFY_OFF_LINE_REQ = llfc_proto::MSG_NOTIFY_OFF_LINE, //1105 通知用户下线
    ID_SEARCH_USER_REQ = llfc_proto::MSG_SEARCH_USER_REQ,     //1201 用户搜索请求
    ID_SEARCH_USER_RSP = llfc_proto::MSG_SEARCH_USER_RSP,     //1202 搜索用户回包
    ID_ADD_FRIEND_REQ = llfc_proto::MSG_ADD_FRIEND_REQ,       //1203 添加好友申请
    ID_ADD_FRIEND_RSP = llfc_proto::MSG_ADD_FRIEND_RSP,       //1204 申请添加好友回复
    ID_HANDLE_FRIEND_REQ = llfc_proto::MSG_HANDLE_FRIEND_REQ, //1205 处理好友申请
    ID_HANDLE_FRIEND_RSP = llfc_proto::MSG_HANDLE_FRIEND_RSP, //1206 处理好友申请回复
    ID_TEXT_CHAT_MSG_REQ  = llfc_proto::MSG_TEXT_CHAT_REQ,    //1301 文本聊天信息请求
    ID_TEXT_CHAT_MSG_RSP  = llfc_proto::MSG_TEXT_CHAT_RSP,    //1302 文本聊天信息回复
    ID_CREATE_PRIVATE_CHAT_REQ = llfc_proto::MSG_CREATE_PRIVATE_CHAT_REQ,   //1303 创建私聊请求
    ID_CREATE_PRIVATE_CHAT_RSP = llfc_proto::MSG_CREATE_PRIVATE_CHAT_RSP,   //1304 创建私聊回复
    ID_LOAD_CHAT_THREAD_REQ = llfc_proto::MSG_LOAD_CHAT_THREAD_REQ,  //1401 加载聊天会话列表
    ID_LOAD_CHAT_THREAD_RSP = llfc_proto::MSG_LOAD_CHAT_THREAD_RSP,  //1402 加载聊天会话列表回复
    ID_LOAD_CHAT_MSG_REQ = llfc_proto::MSG_LOAD_CHAT_MSG_REQ,        //1403 加载聊天消息
    ID_LOAD_CHAT_MSG_RSP = llfc_proto::MSG_LOAD_CHAT_MSG_RSP,        //1404 加载聊天消息回复
    ID_SYNC_USER_MESSAGE_REQ = llfc_proto::MSG_SYNC_USER_MESSAGE_REQ, //1405 统一消息同步请求
    ID_SYNC_USER_MESSAGE_RSP = llfc_proto::MSG_SYNC_USER_MESSAGE_RSP, //1406 统一消息同步回复
    ID_RESOURCE_LOGIN_REQ = llfc_proto::MSG_RESOURCE_LOGIN_REQ,      //1501 资源服务器登录请求
    ID_RESOURCE_LOGIN_RSP = llfc_proto::MSG_RESOURCE_LOGIN_RSP,      //1502 资源服务器登录回复
    ID_CREATE_RESOURCE_MSG_REQ = llfc_proto::MSG_CREATE_RESOURCE_REQ,   //1503 创建资源消息请求
    ID_CREATE_RESOURCE_MSG_RSP = llfc_proto::MSG_CREATE_RESOURCE_RSP,   //1504 创建资源消息回复
    ID_RESOURCE_CHUNK_UPLOAD_REQ = llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_REQ,   //1505 上传资源分片
    ID_RESOURCE_CHUNK_UPLOAD_RSP = llfc_proto::MSG_RESOURCE_CHUNK_UPLOAD_RSP,   //1506 上传资源分片回复
    ID_RESOURCE_UPLOAD_PROGRESS_REQ = llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_REQ,  //1507 查询上传进度
    ID_RESOURCE_UPLOAD_PROGRESS_RSP = llfc_proto::MSG_RESOURCE_UPLOAD_PROGRESS_RSP, //1508 查询上传进度回复
    ID_RESOURCE_DOWN_INFO_REQ = llfc_proto::MSG_RESOURCE_DOWN_INFO_REQ,      //1509 查询资源下载信息请求
    ID_RESOURCE_DOWN_INFO_RSP = llfc_proto::MSG_RESOURCE_DOWN_INFO_RSP,      //1510 查询资源下载信息回复
    ID_RESOURCE_CHUNK_DOWN_REQ = llfc_proto::MSG_RESOURCE_CHUNK_DOWN_REQ,    //1511 按偏移量下载资源分片请求
    ID_RESOURCE_CHUNK_DOWN_RSP = llfc_proto::MSG_RESOURCE_CHUNK_DOWN_RSP,    //1512 按偏移量下载资源分片回复
    ID_UPLOAD_HEAD_ICON_REQ  = llfc_proto::MSG_UPLOAD_HEAD_ICON_REQ,  //1601 上传头像请求
    ID_UPLOAD_HEAD_ICON_RSP  = llfc_proto::MSG_UPLOAD_HEAD_ICON_RSP,  //1602 上传头像回复
    ID_DOWN_LOAD_FILE_REQ = llfc_proto::MSG_DOWN_LOAD_FILE_REQ,      //1603 下载文件请求（旧头像链路）
    ID_DOWN_LOAD_FILE_RSP = llfc_proto::MSG_DOWN_LOAD_FILE_RSP,      //1604 下载文件回复
    ID_NOTIFY_USER_MESSAGE = llfc_proto::MSG_NOTIFY_USER_MESSAGE,    //1701 统一实时消息通知
    ID_REASSIGN_CHAT = llfc_proto::MSG_REASSIGN_CHAT                  //1005 复用当前 token 获取新的 ChatServer
};
Q_DECLARE_METATYPE(ReqId)

/**
 * @brief 服务端错误码（数值唯一来源 proto/protocol_ids.h：20xx 通用 / 21xx 资源）
 *
 * ERR_JSON/ERR_NETWORK 为客户端本地错误（小值），与服务端 20xx 表不同语境。
 */
enum ErrorCodes{
    SUCCESS = llfc_proto::ERR_SUCCESS,
    ERR_JSON = 1, //Json解析失败（本地）
    ERR_NETWORK = 2, //网络错误（本地）
    //服务端投递错误码（20xx 通用表）
    MESSAGE_STORE_FAILED = llfc_proto::ERR_MESSAGE_STORE_FAILED, //2014 消息存储失败（transient，继续重传）
    RECIPIENT_OFFLINE    = llfc_proto::ERR_RECIPIENT_OFFLINE,    //2015 接收方离线
    SERVER_BUSY          = llfc_proto::ERR_SERVER_BUSY,          //2016 服务器繁忙（transient，继续重传）
    MESSAGE_CONFLICT     = llfc_proto::ERR_MESSAGE_CONFLICT,     //2017 消息冲突（permanent，停止重传标 SEND_FAILED）
    RESOURCE_INVALID     = llfc_proto::ERR_RESOURCE_INVALID,     //2019 资源元数据非法（permanent）
    RESOURCE_SIZE_EXCEEDED = llfc_proto::ERR_RESOURCE_SIZE_EXCEEDED, //2020 资源超过类型上限（permanent）
    FRIEND_REQUEST_NOT_FOUND = llfc_proto::ERR_FRIEND_REQUEST_NOT_FOUND,
    FRIEND_REQUEST_HANDLED = llfc_proto::ERR_FRIEND_REQUEST_HANDLED,
    ALREADY_FRIENDS = llfc_proto::ERR_ALREADY_FRIENDS,
    FRIEND_ACTION_INVALID = llfc_proto::ERR_FRIEND_ACTION_INVALID,
    SYNC_CURSOR_INVALID = llfc_proto::ERR_SYNC_CURSOR_INVALID,
    //资源链路错误码（21xx 资源表）
    FILE_OFFSET_INVALID  = llfc_proto::RS_FILE_OFFSET_INVALID,   //2107 分片偏移超前（按响应 server_offset 对齐重发）
    MSG_ID_ERR           = llfc_proto::RS_MSG_ID_ERR,            //2111 消息不存在（permanent，标失败）
    FILE_HASH_MISMATCH   = llfc_proto::RS_FILE_HASH_MISMATCH,    //2112 分片/整文件 SHA-256 校验失败（重传该片/整文件）
    RESOURCE_NOT_READY   = llfc_proto::RS_RESOURCE_NOT_READY,    //2114 资源未就绪（稍后重试或提示）
    RESOURCE_FORBIDDEN   = llfc_proto::RS_RESOURCE_FORBIDDEN,    //2115 无权访问该资源（permanent）
    RESOURCE_STATE_INVALID = llfc_proto::RS_RESOURCE_STATE_INVALID //2116 资源已过期/终态（permanent，标 Expired）
};

enum Modules{
    REGISTERMOD = 0,
    RESETMOD = 1,
    LOGINMOD = 2,
    RECONNECTMOD = 3,
};

enum TipErr{
    TIP_SUCCESS = 0,
    TIP_EMAIL_ERR = 1,
    TIP_PWD_ERR = 2,
    TIP_CONFIRM_ERR = 3,
    TIP_PWD_CONFIRM = 4,
    TIP_VARIFY_ERR = 5,
    TIP_USER_ERR = 6
};

enum ClickLbState{
    Normal = 0,
    Selected = 1
};


extern QString gate_url_prefix;


struct ServerInfo{
public:
    ServerInfo() = default;
    ServerInfo(const ServerInfo& other)
        : _chat_host(other._chat_host), _chat_port(other._chat_port),
          _res_host(other._res_host), _res_port(other._res_port),
          _token(other._token), _uid(other._uid) {}
    QString _chat_host;
    QString _chat_port;
    QString _res_host;
    QString _res_port;
    QString _token;   //Gate 下发的统一登录 token(24h,Chat/Resource 共用同一 token)
    int _uid;
};

Q_DECLARE_METATYPE(ServerInfo)
Q_DECLARE_METATYPE(std::shared_ptr<ServerInfo>)

enum class ChatRole
{

    Self,
    Other
};

enum class MsgType {
    TEXT_MSG = 0, //文本消息
    IMG_MSG = 1,  //图片消息
    FILE_MSG = 3//文件消息,
};

enum class TransferType {
    None,
    Download,  //下载
    Upload     //上传
};

enum class TransferState {
    None,           // 无传输
    Downloading,    // 下载中
    Uploading,      // 上传中
    Paused,         // 暂停
    Completed,      // 完成
    Failed,         // 失败
    Expired         // 资源消息已进入 Failed 终态
};

struct MsgInfo{
    MsgInfo() = default;
    MsgInfo(MsgType msgtype, QString text_or_url, QPixmap pixmap, QString unique_name,
            qint64 total_size, QString content_hash)
    :_msg_type(msgtype), _text_or_url(text_or_url), _preview_pix(pixmap),_unique_name(unique_name),_total_size(total_size),
        _current_size(0),_seq(1),_content_hash(content_hash), _last_confirmed_seq(0),_rsp_size(0), _transfer_state(TransferState::None),
        _transfer_type(TransferType::None)
    {
        _max_seq = ((total_size + MAX_FILE_LEN - 1) / MAX_FILE_LEN);
    }

    MsgType _msg_type;   //消息类型, 文本，图片，文件
    QString _text_or_url;//表示文件和图像的本地路径,文本信息
    QPixmap _preview_pix;//文件和图片的缩略图
    QString _unique_name; //展示文件名（原始文件名；磁盘缓存按 message_id 隔离）
    qint64 _total_size; //文件总大小
    qint64 _current_size; //传输大小（已确认偏移）
    qint64 _seq;          //传输序号（由偏移推导，仅 UI 进度用）
    QString _content_hash;//整文件 SHA-256（小写 hex）
    QVector<QString> _chunk_hashes; //每 32KiB 分片的 SHA-256（后台一次遍历预计算）
    std::set<qint64> _rsp_seqs;      //已经接受的回传序列集合
    std::set<qint64> _flighting_seqs;  //正在发送，但是未收到服务器回复，将来用来做超时重传
    qint64 _last_confirmed_seq;      //最后确认序列
    qint64 _max_seq;                //最大序列号
    qint64 _msg_id;                 //关联的消息id（资源上传/下载的主键）
    qint64 _rsp_size;  //服务器返回实际上传或者下载的大小
    qint64 _thread_id;             // 会话id
    QString _local_download_path;  //下载完成后的本地最终路径（缓存目录内）
    TransferState _transfer_state;  //上传或者下载, 暂停，传输完成
    TransferType  _transfer_type;   //文件类型, 上传或者下载
    int           _sender;          //发送者
    int           _receiver;        //接收者

};
//声明为元对象类型
Q_DECLARE_METATYPE(MsgInfo)
Q_DECLARE_METATYPE(std::shared_ptr<MsgInfo>)

//聊天界面几种模式
enum ChatUIMode{
    SearchMode, //搜索模式
    ChatMode, //聊天模式
    ContactMode, //联系模式
    SettingsMode, //设置模式
};

//自定义QListWidgetItem的几种类型
enum ListItemType{
    CHAT_USER_ITEM, //聊天用户
    CONTACT_USER_ITEM, //联系人用户
    SEARCH_USER_ITEM, //搜索到的用户
    ADD_USER_TIP_ITEM, //提示添加用户
    INVALID_ITEM,  //不可点击条目
    GROUP_TIP_ITEM, //分组提示条目
    LINE_ITEM,  //分割线
    APPLY_FRIEND_ITEM, //好友申请
};

//申请好友标签输入框最低长度
const int MIN_APPLY_LABEL_ED_LEN = 40;

const QString add_prefix = "添加标签 ";

const int  tip_offset = 5;


const std::vector<QString> heads = {
    ":/res/head_1.jpg",
    ":/res/head_2.jpg",
    ":/res/head_3.jpg",
    ":/res/head_4.jpg",
    ":/res/head_5.jpg"
};

const int CHAT_COUNT_PER_PAGE = 13;

enum MsgStatus{
    UN_READ = 0,  //对方未读
    SEND_FAILED = 1,  //发送失败
    READED = 2
};

//服务端 chat_messages.status；资源表本身不重复保存状态。
enum class MessageStatus {
    Pending = 0,
    Published = 1,
    Failed = 2
};

//当前聊天消息类型：文本、图片、文件。
enum class ChatMsgType {
    TEXT = 0,
    PIC = 1,
    FILE = 3
};

//统一同步流中的好友事件类型，不属于聊天消息类型。
enum class UserEventType {
    FRIEND_APPLY = 10,
    FRIEND_ACCEPT = 11,
    FRIEND_REJECT = 12
};

enum class FriendRequestStatus {
    PENDING = 0,
    ACCEPTED = 1,
    REJECTED = 2
};



extern QString generateUniqueFileName(const QString& originalName);

extern QString generateUniqueIconName();

struct DownloadInfo {
    QString _name;
    int _total_size;
    int _current_size;
    int _seq;
    QString _client_path;
};

//后台一次 32KiB 遍历，同时产出整文件 SHA-256 与每片 SHA-256；
//文件不可读返回 false（out 参数保持为空）
extern bool calculateFileSha256(const QString& filePath, QString& content_hash,
    QVector<QString>& chunk_hashes);
//整文件 SHA-256（校验下载结果用）
extern QString calculateFileSha256Only(const QString& filePath);
extern     QPixmap CreateLoadingPlaceholder(int width = 200, int height = 200);
//根据扩展名猜测 MIME 类型（1503 创建请求的 mime_type 字段）
extern QString guessMimeType(const QString& fileName);

#endif // GLOBAL_H
