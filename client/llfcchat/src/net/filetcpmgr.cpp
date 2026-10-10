#include "filetcpmgr.h"
#include "usermgr.h"
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QCoreApplication>

#include "protocol_ids.h"
#include "resource_wire.h"

FileTcpMgr::FileTcpMgr(QObject* parent) : QObject(parent),
	_socket(this), //父子亲缘：moveToThread 时随本对象一起迁移
	_host(""), _port(0), _b_recv_pending(false), _message_type(0), _message_len(0),
	_bytes_sent(0), _pending(false), _authenticated(false), _manual_close(false),
	_reconnect_timer(new QTimer(this)), _reconnect_attempt(0),
	_engine(0), _send_high_watermark(4 * 1024 * 1024),
	_auth_timer(new QTimer(this)), _auth_timeout_ms(15000),
	_send_queue_bytes(0), _send_queue_limit_bytes(64 * 1024 * 1024),
	_auth_uid(0), _login_generation(0), //atomic<int> 值初始化
	_reconnect_initial_ms(1000), _reconnect_max_ms(30000)
{
	registerMetaType();
	//传输引擎：File 线程独占（配置取 config.ini [Resource] 段）
	_engine = new TransferEngine(loadEngineConfig(), this);
	_engine->SetSendCallback([this](quint16 msg_id, const QByteArray& payload) {
		//引擎发帧 → 统一经 sig_send_data 信号（鉴权门禁在 slot_send_data；
		//信号路由让单测 spy 与跨线程调用看到一致的发送序列）
		emit sig_send_data(static_cast<ReqId>(msg_id), payload);
	});
	//引擎快照转发（跨线程由 Qt queued 投递到 GUI/outbox）：转发时附加
	//owner uid + 登录代次，GUI 核验后才能据此写本地库（防旧快照串写）
	connect(_engine, &TransferEngine::sig_transfer_updated, this,
		[this](TransferSnapshot s) {
			s.owner_uid = _auth_uid;
			s.login_generation = _login_generation.load(std::memory_order_acquire);
			emit sig_transfer_updated(s);
		});
	connect(_engine, &TransferEngine::sig_transfer_finished, this,
		[this](TransferSnapshot s) {
			s.owner_uid = _auth_uid;
			s.login_generation = _login_generation.load(std::memory_order_acquire);
			emit sig_transfer_finished(s);
		});
	//30s 无回包判定连接死亡：立即按异常断开处理（退避重连）
	connect(_engine, &TransferEngine::sig_request_reconnect, this, [this]() {
		if (_manual_close) {
			return;
		}
		_authenticated = false;
		_engine->OnConnectionDown();
		_socket.abort();
		scheduleReconnect();
	});

	//连接起点：新连接不允许继承上一连接的鉴权态
	QObject::connect(&_socket, &QTcpSocket::connected, this, [this]() {
		qDebug() << "[FileTcpMgr] connected, sending 1501";
		_authenticated = false;
		_auth_timer->stop(); //连接看门狗停表，1501 重起鉴权计时
		sendResourceLogin();
	});

	//资源帧上限：32KiB 分片 Base64 + JSON 包络 ≈ 45KiB，留足余量取 1MiB；
	//超限/0 长度/msg_id 非法一律判定协议破坏，重置连接
	QObject::connect(&_socket, &QTcpSocket::readyRead, this, [this]() {
		_buffer.append(_socket.readAll());
		forever {
			if (!_b_recv_pending) {
				if (_buffer.size() < FILE_UPLOAD_HEAD_LEN) {
					return;
				}
				QDataStream stream(_buffer);
				stream.setVersion(QDataStream::Qt_5_0);
				stream >> _message_type >> _message_len;
				//头部合法性先验证再消费：0 长度/超上限/超 int 范围/msg_id
				//越界（ChatServer 侧 kMaxMsgId 同规则）都按坏连接处理
				if (_message_len == 0
					|| _message_len > static_cast<quint32>(llfc_wire::kMaxFrameBody)
					|| _message_type == 0
					|| _message_type > llfc_proto::kMaxMsgId) {
					qWarning() << "[FileTcpMgr] bad frame header, reset: id="
						<< _message_type << " len=" << _message_len;
					resetConnection();
					return;
				}
				_buffer.remove(0, FILE_UPLOAD_HEAD_LEN); //就地消费
			}
			if (_buffer.size() < static_cast<int>(_message_len)) {
				_b_recv_pending = true;
				return;
			}
			_b_recv_pending = false;
			QByteArray messageBody = _buffer.left(static_cast<int>(_message_len));
			_buffer.remove(0, static_cast<int>(_message_len));
			handleMsg(ReqId(_message_type), static_cast<int>(_message_len), messageBody);
		}
	});

	//处理错误（适用于Qt 5.15之前的版本）：首连失败提示登录流程；
	//ConnectionRefused/HostNotFound 不一定触发 disconnected，这里直接进退避重连
	QObject::connect(&_socket,
		static_cast<void (QTcpSocket::*)(QTcpSocket::SocketError)>(&QTcpSocket::error),
		this,
		[this](QTcpSocket::SocketError socketError) {
			qWarning() << "[FileTcpMgr] socket error:" << socketError
				<< _socket.errorString();
			if (_manual_close) {
				return;
			}
			const bool first_connect_fail =
				(socketError == QTcpSocket::ConnectionRefusedError
					|| socketError == QTcpSocket::HostNotFoundError);
			if (first_connect_fail && !_authenticated) {
				emit sig_con_success(false);
			}
			resetConnection();
			scheduleReconnect();
		});

	//断开：主动关闭只复位；异常断开清收发状态并退避重连
	QObject::connect(&_socket, &QTcpSocket::disconnected, this, [this]() {
		qDebug() << "[FileTcpMgr] disconnected, manual=" << _manual_close;
		const bool was_authenticated = _authenticated;
		_authenticated = false;
		//收发状态复位：半包、发送队列、鉴权态、看门狗全部作废
		_b_recv_pending = false;
		_message_type = 0;
		_message_len = 0;
		_buffer.clear();
		_send_queue.clear();
		_send_queue_bytes = 0;
		_current_block.clear();
		_bytes_sent = 0;
		_pending = false;
		_auth_timer->stop();
		if (_engine) {
			_engine->OnConnectionDown();
		}
		if (_manual_close) {
			return;
		}
		if (!was_authenticated) {
			emit sig_con_success(false); //首连失败：登录流程提示
		}
		scheduleReconnect();
	});

	//发送轮转：write 返回值=已接受字节。正常 Qt 整帧接受（一次 write 完成）；
	//罕见部分接受时只补写未被接受的后缀；缓冲低于高水位即继续把队列帧
	//排入 Qt 缓冲（流水背压，不必等上一帧完全冲刷）
	QObject::connect(&_socket, &QTcpSocket::bytesWritten, this, [this](qint64 bytes) {
		Q_UNUSED(bytes);
		flushPendingSuffix();
		tryDequeueSend();
	});

	QObject::connect(this, &FileTcpMgr::sig_close, this, &FileTcpMgr::slot_tcp_close);
	QObject::connect(this, &FileTcpMgr::sig_send_data, this, &FileTcpMgr::slot_send_data);
	//跨线程资源传输入口（File 线程执行）
	QObject::connect(this, &FileTcpMgr::sig_internal_start_upload,
		this, &FileTcpMgr::slot_internal_start_upload);
	QObject::connect(this, &FileTcpMgr::sig_internal_start_download,
		this, &FileTcpMgr::slot_internal_start_download);
	QObject::connect(this, &FileTcpMgr::sig_internal_pause,
		this, &FileTcpMgr::slot_internal_pause);
	QObject::connect(this, &FileTcpMgr::sig_internal_resume,
		this, &FileTcpMgr::slot_internal_resume);
	QObject::connect(this, &FileTcpMgr::sig_internal_cancel,
		this, &FileTcpMgr::slot_internal_cancel);
	QObject::connect(this, &FileTcpMgr::sig_internal_frame,
		this, &FileTcpMgr::slot_internal_frame);
	QObject::connect(this, &FileTcpMgr::sig_internal_connect,
		this, &FileTcpMgr::slot_tcp_connect);
	//1502 结果与引擎会合（自连接：测试直接 emit 同样生效）
	QObject::connect(this, &FileTcpMgr::sig_resource_login_success,
		this, &FileTcpMgr::slot_engine_connection_up);
	QObject::connect(this, &FileTcpMgr::sig_resource_login_failed,
		this, &FileTcpMgr::slot_engine_connection_down);
	//重连退避定时器（子对象：随 moveToThread 一起迁移）
	_reconnect_timer->setSingleShot(true);
	QObject::connect(_reconnect_timer, &QTimer::timeout,
		this, &FileTcpMgr::slot_reconnect_timeout);
	//1501 鉴权看门狗：发出 1501 后 _auth_timeout_ms 无 1502 回包按连接死亡重连
	_auth_timer->setSingleShot(true);
	QObject::connect(_auth_timer, &QTimer::timeout, this, [this]() {
		if (_manual_close || _authenticated) {
			return;
		}
		qWarning() << "[FileTcpMgr] resource auth timeout, reconnecting";
		resetConnection();
		scheduleReconnect();
	});

	initHandlers();
}

FileTcpMgr::~FileTcpMgr() {
	_authenticated = false;
}

TransferEngine::Config FileTcpMgr::loadEngineConfig()
{
	//config.ini [Resource] 段：窗口/超时/重连退避（缺省即引擎默认）
	TransferEngine::Config cfg;
	QString app_path = QCoreApplication::applicationDirPath();
	QString config_path = QDir::toNativeSeparators(
		app_path + QDir::separator() + "config.ini");
	QSettings settings(config_path, QSettings::IniFormat);
	bool ok = false;
	qint64 v = settings.value("Resource/MaxActiveUploads",
		cfg.max_active_uploads).toLongLong(&ok);
	if (ok && v > 0) cfg.max_active_uploads = static_cast<int>(v);
	v = settings.value("Resource/MaxActiveDownloads",
		cfg.max_active_downloads).toLongLong(&ok);
	if (ok && v > 0) cfg.max_active_downloads = static_cast<int>(v);
	v = settings.value("Resource/MaxInflightChunks",
		cfg.max_inflight_chunks).toLongLong(&ok);
	if (ok && v > 0) cfg.max_inflight_chunks = static_cast<int>(v);
	v = settings.value("Resource/AckTimeoutMs", cfg.ack_timeout_ms).toLongLong(&ok);
	if (ok && v > 0) cfg.ack_timeout_ms = static_cast<int>(v);
	v = settings.value("Resource/BackoffInitialMs",
		cfg.backoff_initial_ms).toLongLong(&ok);
	if (ok && v > 0) cfg.backoff_initial_ms = static_cast<int>(v);
	v = settings.value("Resource/BackoffMaxMs", cfg.backoff_max_ms).toLongLong(&ok);
	if (ok && v > 0) cfg.backoff_max_ms = static_cast<int>(v);
	//P1c：wire 分片单位不可配置——cfg.chunk_size 由引擎固定为共享
	//llfc_wire::kResourceChunkSize（256KiB）。旧 Resource/ChunkSizeKB
	//配置键（含 32KiB 旧值）一律忽略；窗口/超时/退避仍可配。
	v = settings.value("Resource/TickIntervalMs",
		cfg.tick_interval_ms).toLongLong(&ok);
	if (ok && v > 0) cfg.tick_interval_ms = static_cast<int>(v);
	v = settings.value("Resource/ReconnectInitialMs", 1000).toLongLong(&ok);
	if (ok && v > 0) _reconnect_initial_ms = v;
	v = settings.value("Resource/ReconnectMaxMs", 30000).toLongLong(&ok);
	if (ok && v > 0) _reconnect_max_ms = v;
	v = settings.value("Resource/SendHighWatermarkBytes",
		_send_high_watermark).toLongLong(&ok);
	if (ok && v > 0) _send_high_watermark = v;
	return cfg;
}

void FileTcpMgr::registerMetaType()
{
	// 注册所有自定义类型
	qRegisterMetaType<ServerInfo>("ServerInfo");
	qRegisterMetaType<std::shared_ptr<ServerInfo>>("std::shared_ptr<ServerInfo>");
	qRegisterMetaType<SearchInfo>("SearchInfo");
	qRegisterMetaType<std::shared_ptr<SearchInfo>>("std::shared_ptr<SearchInfo>");
	qRegisterMetaType<AddFriendApply>("AddFriendApply");
	qRegisterMetaType<std::shared_ptr<AddFriendApply>>("std::shared_ptr<AddFriendApply>");
	qRegisterMetaType<ApplyInfo>("ApplyInfo");
	qRegisterMetaType<std::shared_ptr<AuthInfo>>("std::shared_ptr<AuthInfo>");
	qRegisterMetaType<AuthRsp>("AuthRsp");
	qRegisterMetaType<std::shared_ptr<AuthRsp>>("std::shared_ptr<AuthRsp>");
	qRegisterMetaType<UserInfo>("UserInfo");
	qRegisterMetaType<std::vector<std::shared_ptr<TextChatData>>>(
		"std::vector<std::shared_ptr<TextChatData>>");
	qRegisterMetaType<std::vector<std::shared_ptr<ChatThreadData>>>(
		"std::vector<std::shared_ptr<ChatThreadData>>");
	qRegisterMetaType<std::shared_ptr<ChatThreadData>>("std::shared_ptr<ChatThreadData>");
	qRegisterMetaType<ReqId>("ReqId");
	qRegisterMetaType<MsgInfo>("MsgInfo");
	qRegisterMetaType<std::shared_ptr<MsgInfo>>("std::shared_ptr<MsgInfo>");
	RegisterResourceTransferMetaTypes();
}

void FileTcpMgr::handleMsg(ReqId id, int len, QByteArray data)
{
	auto find_iter = _handlers.find(id);
	if (find_iter == _handlers.end()) {
		qDebug() << "not found id [" << id << "] to handle";
		return;
	}
	(this->*find_iter.value())(id, len, data);
}

void FileTcpMgr::slot_send_data(ReqId reqId, QByteArray dataBytes)
{
	//鉴权前禁止发送任何业务帧（仅允许 1501 登录请求）
	if (!_authenticated && reqId != ID_RESOURCE_LOGIN_REQ) {
		qWarning() << "[FileTcpMgr] dropping frame id=" << reqId
			<< " before resource authentication";
		return;
	}
	uint16_t id = reqId;
	quint32 len = static_cast<quint32>(dataBytes.length());
	QByteArray block;
	QDataStream out(&block, QIODevice::WriteOnly);
	out.setByteOrder(QDataStream::BigEndian);
	out << id << len;
	block.append(dataBytes);

	//队列字节上界：真实有界（满则判定连接已死，重置后退避重连）
	if (_send_queue_bytes + block.size() > _send_queue_limit_bytes) {
		qWarning() << "[FileTcpMgr] send queue overflow, reset connection";
		resetConnection();
		scheduleReconnect();
		return;
	}
	_send_queue.enqueue(block);
	_send_queue_bytes += block.size();
	tryDequeueSend();
}

void FileTcpMgr::tryDequeueSend()
{
	//流水背压：Qt 缓冲低于高水位即可继续入写（部分接受的后缀已冲刷或
	//本就无后缀），不等待上一帧完全冲刷
	while (!_pending && !_send_queue.isEmpty()
		&& _socket.bytesToWrite() < _send_high_watermark) {
		_current_block = _send_queue.dequeue();
		_send_queue_bytes -= _current_block.size();
		_bytes_sent = 0;
		const qint64 accepted = _socket.write(_current_block);
		if (accepted < 0) {
			qWarning() << "[FileTcpMgr] write failed:" << _socket.errorString();
			//write<0：不得丢当前帧继续下一帧——立即按连接死亡处理
			resetConnection();
			scheduleReconnect();
			return;
		}
		if (accepted < _current_block.size()) {
			//罕见部分接受：只保留未接受后缀等待补写
			_current_block = _current_block.mid(static_cast<int>(accepted));
			_bytes_sent = accepted;
			_pending = true;
			return;
		}
		_current_block.clear(); //整帧已被 Qt 接受
	}
}

void FileTcpMgr::flushPendingSuffix()
{
	if (!_pending) {
		return;
	}
	int retries = 0;
	while (!_current_block.isEmpty()) {
		const qint64 accepted = _socket.write(_current_block);
		if (accepted < 0) {
			qWarning() << "[FileTcpMgr] suffix write failed:"
				<< _socket.errorString();
			//后缀写失败：立即按连接死亡处理，不丢尾续发
			resetConnection();
			scheduleReconnect();
			return;
		}
		if (accepted == 0 && ++retries >= 3) {
			//0 接受且有界重试仍无进展：按连接失败处理
			qWarning() << "[FileTcpMgr] suffix write stalled, reset connection";
			resetConnection();
			scheduleReconnect();
			return;
		}
		if (accepted < _current_block.size()) {
			_current_block = _current_block.mid(static_cast<int>(accepted));
			_bytes_sent += accepted;
			return; //仍部分接受，等下次 bytesWritten
		}
		_current_block.clear();
		_pending = false;
	}
	_pending = false;
	_bytes_sent = 0;
}

void FileTcpMgr::resetConnection()
{
	//半包/发送队列/鉴权态/看门狗全部复位（不触发重连本身）
	_authenticated = false;
	_b_recv_pending = false;
	_message_type = 0;
	_message_len = 0;
	_buffer.clear();
	_send_queue.clear();
	_send_queue_bytes = 0;
	_current_block.clear();
	_bytes_sent = 0;
	_pending = false;
	_auth_timer->stop();
	_socket.abort();
	if (_engine) {
		_engine->OnConnectionDown();
	}
}

void FileTcpMgr::slot_tcp_connect(std::shared_ptr<ServerInfo> si)
{
	if (!si) {
		return;
	}
	//值快照：鉴权只依赖连接时捕获的 uid/token，不在线程间再读 UserMgr
	const int new_uid = si->_uid;
	const QString new_host = si->_res_host;
	const uint16_t new_port = static_cast<uint16_t>(si->_res_port.toUInt());
	const bool same_target = (new_uid == _auth_uid && new_host == _host
		&& new_port == _port);
	const bool socket_live = (_socket.state() == QAbstractSocket::ConnectedState
		|| _socket.state() == QAbstractSocket::ConnectingState);
	if (same_target && socket_live) {
		//同 uid 重复 connect 信号：幂等忽略，不重启上传/不重建连接
		return;
	}
	if (new_uid > 0 && new_uid != _auth_uid && _auth_uid > 0) {
		//用户代次切换：静默 Reset 引擎/收发/凭据（A 的传输不得在 B 登录
		//后被重启），再以新身份连接
		qDebug() << "[FileTcpMgr] user switch" << _auth_uid << "->" << new_uid;
		slot_tcp_close();
	}
	_server_info = si;
	_auth_uid = new_uid;
	_auth_token = si->_token;
	_manual_close = false;
	_reconnect_attempt = 0;
	_host = new_host;
	_port = new_port;
	//连接超时看门狗：connectToHost 之前启动，connected 后停表转鉴权计时
	_auth_timer->start(_auth_timeout_ms);
	_socket.abort();
	_socket.connectToHost(_host, _port);
}

void FileTcpMgr::sendResourceLogin()
{
	//1501 只用连接时捕获的凭据快照（slot_tcp_connect 值拷贝），不读 UserMgr；
	//connected 已停连接看门狗，这里重新起 15s 鉴权计时
	if (_auth_uid <= 0 || _auth_token.isEmpty()) {
		emit sig_resource_login_failed(tr("缺少资源服务器鉴权凭证"));
		return;
	}
	QJsonObject jsonObj;
	jsonObj["uid"] = _auth_uid;
	jsonObj["token"] = _auth_token;
	const QByteArray jsonData =
		QJsonDocument(jsonObj).toJson(QJsonDocument::Compact);
	slot_send_data(ReqId::ID_RESOURCE_LOGIN_REQ, jsonData);
	_auth_timer->start(_auth_timeout_ms);
}

void FileTcpMgr::scheduleReconnect()
{
	if (_manual_close || _reconnect_timer->isActive()) {
		return; //去重：abort 触发的 error/disconnected 不得递归排重连
	}
	//退避 1s→2s→…→30s 封顶
	qint64 delay = _reconnect_initial_ms;
	for (int i = 0; i < _reconnect_attempt && i < 30; ++i) {
		delay *= 2;
		if (delay >= _reconnect_max_ms) {
			delay = _reconnect_max_ms;
			break;
		}
	}
	if (delay > _reconnect_max_ms) {
		delay = _reconnect_max_ms;
	}
	++_reconnect_attempt;
	_reconnect_timer->start(static_cast<int>(delay));
}

void FileTcpMgr::slot_reconnect_timeout()
{
	if (_manual_close || _host.isEmpty() || _port == 0) {
		return;
	}
	qDebug() << "[FileTcpMgr] reconnecting (attempt=" << _reconnect_attempt << ")";
	_socket.abort();
	_socket.connectToHost(_host, _port);
}

void FileTcpMgr::slot_tcp_close()
{
	//主动关闭/登出/用户代次切换：静默重置引擎（关闭全部 QFile、清空两
	//方向登记与等待队列——A 的传输不得在 B 登录后被重启），并清凭据
	_manual_close = true;
	_reconnect_timer->stop();
	_auth_timer->stop();
	_authenticated = false;
	_b_recv_pending = false;
	_message_type = 0;
	_message_len = 0;
	_buffer.clear();
	_send_queue.clear();
	_send_queue_bytes = 0;
	_current_block.clear();
	_bytes_sent = 0;
	_pending = false;
	if (_engine) {
		_engine->Reset();
	}
	_server_info.reset();
	_auth_uid = 0;
	_auth_token.clear();
	_login_generation.fetch_add(1, std::memory_order_release); //旧代次快照作废
	_host.clear();
	_port = 0;
	_socket.close();
}

void FileTcpMgr::CloseConnection()
{
	emit sig_close();
}

void FileTcpMgr::SendData(ReqId reqId, QByteArray data)
{
	emit sig_send_data(reqId, data);
}

void FileTcpMgr::slot_engine_connection_up()
{
	//1502 成功：引擎恢复（活跃上传重发 1507、下载从 .part 续传）
	if (_engine) {
		_engine->OnConnectionAuthenticated();
	}
}

void FileTcpMgr::slot_engine_connection_down()
{
	if (_engine) {
		_engine->OnConnectionDown();
	}
}

//---- 资源传输入口（跨线程 → File 线程 → 引擎）----

void FileTcpMgr::PostStartUpload(const UploadRequest& req)
{
	emit sig_internal_start_upload(req);
}

void FileTcpMgr::PostStartDownload(const DownloadRequest& req)
{
	emit sig_internal_start_download(req);
}

void FileTcpMgr::PostPauseTransfer(int direction, qint64 message_id)
{
	emit sig_internal_pause(direction, message_id);
}

void FileTcpMgr::PostResumeTransfer(int direction, qint64 message_id)
{
	emit sig_internal_resume(direction, message_id);
}

void FileTcpMgr::PostCancelTransfer(int direction, qint64 message_id)
{
	emit sig_internal_cancel(direction, message_id);
}

void FileTcpMgr::PostFrame(quint16 msg_id, const QByteArray& payload)
{
	emit sig_internal_frame(msg_id, payload);
}

void FileTcpMgr::slot_internal_start_upload(UploadRequest req)
{
	if (_engine) {
		_engine->StartUpload(req);
	}
}

void FileTcpMgr::slot_internal_start_download(DownloadRequest req)
{
	if (_engine) {
		_engine->StartDownload(req);
	}
}

void FileTcpMgr::slot_internal_pause(int direction, qint64 message_id)
{
	if (_engine) {
		_engine->PauseTransfer(direction, message_id);
	}
}

void FileTcpMgr::slot_internal_resume(int direction, qint64 message_id)
{
	if (_engine) {
		_engine->ResumeTransfer(direction, message_id);
	}
}

void FileTcpMgr::slot_internal_cancel(int direction, qint64 message_id)
{
	if (_engine) {
		_engine->CancelTransfer(direction, message_id);
	}
}

void FileTcpMgr::slot_internal_frame(quint16 msg_id, QByteArray payload)
{
	if (_engine) {
		_engine->OnFrame(msg_id, payload);
	}
}

void FileTcpMgr::initHandlers()
{
	//1502 Resource 登录回复：鉴权成功后才允许发送业务帧
	_handlers.insert(ID_RESOURCE_LOGIN_RSP, &FileTcpMgr::handleResourceLoginRsp);
	//1506/1508/1510/1512/1514 原样喂引擎（确认/回退/秒传/超时/退避全在引擎内闭环）
	_handlers.insert(ID_RESOURCE_CHUNK_UPLOAD_RSP, &FileTcpMgr::handleEngineFrameRsp);
	_handlers.insert(ID_RESOURCE_UPLOAD_PROGRESS_RSP, &FileTcpMgr::handleEngineFrameRsp);
	_handlers.insert(ID_RESOURCE_DOWN_INFO_RSP, &FileTcpMgr::handleEngineFrameRsp);
	_handlers.insert(ID_RESOURCE_CHUNK_DOWN_RSP, &FileTcpMgr::handleEngineFrameRsp);
	_handlers.insert(ID_RESOURCE_INSTANT_PROOF_RSP, &FileTcpMgr::handleEngineFrameRsp);
	// ---------- 头像通道（1601/1602/1603/1604，旧协议保持不变） ----------
	_handlers.insert(ID_UPLOAD_HEAD_ICON_RSP, &FileTcpMgr::handleUploadHeadIconRsp);
	_handlers.insert(ID_DOWN_LOAD_FILE_RSP, &FileTcpMgr::handleDownloadFileRsp);
}


void FileTcpMgr::handleResourceLoginRsp(ReqId id, int len, QByteArray data)
{
	Q_UNUSED(len);
	Q_UNUSED(id);
	QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
	if (jsonDoc.isNull() || !jsonDoc.isObject()) {
		_authenticated = false;
		emit sig_resource_login_failed(tr("资源服务器鉴权响应解析失败"));
		return;
	}
	QJsonObject jsonObj = jsonDoc.object();
	_auth_timer->stop();
	int err = jsonObj.contains("error") ? jsonObj["error"].toInt()
		: ErrorCodes::ERR_JSON;
	if (err == static_cast<int>(llfc_proto::ERR_TOKEN_INVALID)) {
		//2010：Token 失效——唯一需要主界面登出的场景
		_authenticated = false;
		emit sig_resource_token_invalid();
		return;
	}
	if (err != ErrorCodes::SUCCESS) {
		_authenticated = false;
		qDebug() << "[FileTcpMgr] resource login failed, error=" << err;
		emit sig_resource_login_failed(tr("资源服务器鉴权失败"));
		return;
	}
	_authenticated = true;
	//注意：1502 成功不递增登录代次——同 session 的普通重连若 bump 代次，
	//重连前已 Published 的排队终态快照会被 Outbox/GUI 代次门禁丢弃，而
	//引擎 Completed 不重发、_uploading 已登记阻止 resume → 永久 Sending。
	//代次仅在主动 slot_tcp_close（含 uid 切换复用）等会话边界递增。
	qDebug() << "[FileTcpMgr] resource login success";
	emit sig_resource_login_success();
}

void FileTcpMgr::handleEngineFrameRsp(ReqId id, int len, QByteArray data)
{
	//1506/1508/1510/1512/1514：原样交给引擎（分片确认/回退/秒传证明/超时全在引擎内闭环）
	Q_UNUSED(len);
	slot_internal_frame(static_cast<quint16>(id), data);
}

void FileTcpMgr::handleUploadHeadIconRsp(ReqId id, int len, QByteArray data)
{
	Q_UNUSED(len);
	Q_UNUSED(id);
	QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
	if (jsonDoc.isNull()) {
		qWarning() << "[FileTcpMgr] icon rsp json parse failed";
		return;
	}
	QJsonObject recvObj = jsonDoc.object();
	if (!recvObj.contains("error")) {
		return;
	}
	int err = recvObj["error"].toInt();
	if (err != ErrorCodes::SUCCESS) {
		qWarning() << "[FileTcpMgr] icon upload failed, err=" << err;
		return;
	}

	auto md5 = recvObj["md5"].toString();
	auto seq = recvObj["seq"].toInt();
	auto trans_size = recvObj["trans_size"].toInt();
	auto total_size = recvObj["total_size"].toInt();
	auto name = recvObj["name"].toString();

	if (total_size == trans_size) {
		UserMgr::GetInstance()->RmvUploadFile(name);
		return;
	}

	auto file_info = UserMgr::GetInstance()->GetUploadInfoByName(name);
	if (!file_info) {
		return;
	}

	QFile file(file_info->filePath());
	if (!file.open(QIODevice::ReadOnly)) {
		qWarning() << "[FileTcpMgr] could not open icon file: " << file.errorString();
		return;
	}
	file.seek(trans_size);
	QByteArray buffer;
	seq++;
	buffer = file.read(MAX_FILE_LEN);
	QJsonObject sendObj;
	sendObj["md5"] = md5;
	sendObj["name"] = name;
	sendObj["seq"] = seq;
	sendObj["trans_size"] = buffer.size() + (seq - 1) * MAX_FILE_LEN;
	sendObj["total_size"] = total_size;
	sendObj["last"] = (buffer.size() + (seq - 1) * MAX_FILE_LEN >= total_size) ? 1 : 0;
	sendObj["data"] = QString::fromLatin1(buffer.toBase64());
	sendObj["last_seq"] = recvObj["last_seq"].toInt();
	SendData(ID_UPLOAD_HEAD_ICON_REQ, QJsonDocument(sendObj).toJson());
	file.close();
}

void FileTcpMgr::handleDownloadFileRsp(ReqId id, int len, QByteArray data)
{
	Q_UNUSED(len);
	Q_UNUSED(id);
	QJsonDocument jsonDoc = QJsonDocument::fromJson(data);
	if (jsonDoc.isNull()) {
		return;
	}
	QJsonObject jsonObj = jsonDoc.object();
	if (!jsonObj.contains("error")) {
		return;
	}
	int err = jsonObj["error"].toInt();
	if (err != ErrorCodes::SUCCESS) {
		qWarning() << "[FileTcpMgr] avatar download failed, error=" << err;
		return;
	}

	QString base64Data = jsonObj["data"].toString();
	QString clientPath = jsonObj["client_path"].toString();
	int seq = jsonObj["seq"].toInt();
	bool is_last = jsonObj["is_last"].toBool();
	qint64 total_size = jsonObj["total_size"].toString().toLongLong();
	qint64 current_size = jsonObj["current_size"].toString().toLongLong();
	QString name = jsonObj["name"].toString();
	QString req_type = jsonObj["req_type"].toString();

	auto file_info = UserMgr::GetInstance()->GetDownloadInfo(name);
	if (file_info == nullptr) {
		return;
	}

	file_info->_current_size = static_cast<int>(current_size);
	file_info->_total_size = static_cast<int>(total_size);

	QByteArray decodedData = QByteArray::fromBase64(base64Data.toUtf8());
	QFile file(clientPath);
	QIODevice::OpenMode mode = (seq == 1) ? QIODevice::WriteOnly
		: (QIODevice::WriteOnly | QIODevice::Append);
	if (!file.open(mode)) {
		qWarning() << "[FileTcpMgr] failed to open avatar file:" << clientPath;
		return;
	}
	if (file.write(decodedData) != decodedData.size()) {
		qWarning() << "[FileTcpMgr] avatar write short";
	}
	file.close();

	if (is_last) {
		UserMgr::GetInstance()->RmvDownloadFile(name);
		if (req_type == "self_icon") {
			emit sig_reset_label_icon(clientPath);
		}
	}
	else {
		file_info->_seq = seq + 1;
		FileTcpMgr::GetInstance()->SendDownloadInfo(file_info, req_type);
	}
}

void FileTcpMgr::SendDownloadInfo(std::shared_ptr<DownloadInfo> download, QString req_type) {
	QJsonObject jsonObj;
	jsonObj["name"] = download->_name;
	jsonObj["seq"] = download->_seq;
	jsonObj["trans_size"] = 0;
	jsonObj["total_size"] = 0;
	jsonObj["client_path"] = download->_client_path;
	jsonObj["req_type"] = req_type;
	SendData(ID_DOWN_LOAD_FILE_REQ, QJsonDocument(jsonObj).toJson());
}

FileTcpThread::FileTcpThread()
{
	_file_tcp_thread = new QThread();
	FileTcpMgr::GetInstance()->moveToThread(_file_tcp_thread);
	QObject::connect(_file_tcp_thread, &QThread::finished, _file_tcp_thread, &QObject::deleteLater);
	_file_tcp_thread->start();
}

FileTcpThread::~FileTcpThread()
{
	_file_tcp_thread->quit();
	_file_tcp_thread->wait();
}
