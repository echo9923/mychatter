#include "FileBubble.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QFileDialog>
#include <QUrl>
#include <QMessageBox>

FileBubble::FileBubble(const QString& file_name, qint64 total_size, ChatRole role,
    QWidget* parent)
    : BubbleFrame(role, parent), m_state(TransferState::None),
    m_file_name(file_name), m_local_path(QString()), m_total_size(total_size)
{
    QWidget* body = new QWidget(this);
    QVBoxLayout* vLayout = new QVBoxLayout(body);
    vLayout->setContentsMargins(10, 8, 10, 8);
    vLayout->setSpacing(4);

    //首行：扩展名图标 + 文件名 + 大小
    QHBoxLayout* topLayout = new QHBoxLayout();
    topLayout->setSpacing(8);
    m_iconLabel = new QLabel(body);
    m_iconLabel->setFixedSize(32, 32);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    //按扩展名给出简易图标文字（避免引入图标资源依赖）
    const QString suffix = QFileInfo(file_name).suffix().toUpper();
    m_iconLabel->setText(suffix.isEmpty() ? QStringLiteral("FILE") : suffix.left(4));
    m_iconLabel->setStyleSheet(
        "QLabel{background:#dcdfe6;border-radius:4px;color:#5a5e66;"
        "font-weight:bold;font-size:10px;}");
    m_nameLabel = new QLabel(file_name, body);
    m_nameLabel->setMaximumWidth(260);
    m_nameLabel->setToolTip(file_name);
    m_sizeLabel = new QLabel(humanSize(total_size), body);
    m_sizeLabel->setStyleSheet("QLabel{color:#909399;font-size:11px;}");
    topLayout->addWidget(m_iconLabel);
    topLayout->addWidget(m_nameLabel, 1);
    topLayout->addWidget(m_sizeLabel);
    vLayout->addLayout(topLayout);

    //进度条（下载中可见）
    m_progressBar = new QProgressBar(body);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->hide();
    vLayout->addWidget(m_progressBar);

    //按钮组：下载/暂停/继续/打开/另存为
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(6);
    m_downloadBtn = new QPushButton(QStringLiteral("下载"), body);
    m_pauseBtn = new QPushButton(QStringLiteral("暂停"), body);
    m_resumeBtn = new QPushButton(QStringLiteral("继续"), body);
    m_openBtn = new QPushButton(QStringLiteral("打开"), body);
    m_saveAsBtn = new QPushButton(QStringLiteral("另存为"), body);
    btnLayout->addWidget(m_downloadBtn);
    btnLayout->addWidget(m_pauseBtn);
    btnLayout->addWidget(m_resumeBtn);
    btnLayout->addWidget(m_openBtn);
    btnLayout->addWidget(m_saveAsBtn);
    btnLayout->addStretch(1);
    vLayout->addLayout(btnLayout);

    setWidget(body);
    refreshButtons();

    connect(m_downloadBtn, &QPushButton::clicked, this, [this]() {
        if (_msg_info) {
            emit downloadRequested(_msg_info->_unique_name);
        }
    });
    connect(m_pauseBtn, &QPushButton::clicked, this, [this]() {
        if (_msg_info) {
            emit pauseRequested(_msg_info->_unique_name, TransferType::Download);
        }
    });
    connect(m_resumeBtn, &QPushButton::clicked, this, [this]() {
        if (_msg_info) {
            emit resumeRequested(_msg_info->_unique_name, TransferType::Download);
        }
    });
    connect(m_openBtn, &QPushButton::clicked, this, &FileBubble::onOpenClicked);
    connect(m_saveAsBtn, &QPushButton::clicked, this, &FileBubble::onSaveAsClicked);
}

void FileBubble::setMsgInfo(std::shared_ptr<MsgInfo> msg)
{
    _msg_info = msg;
    if (msg) {
        m_file_name = msg->_unique_name;
        m_nameLabel->setText(m_file_name);
        m_nameLabel->setToolTip(m_file_name);
        if (!msg->_local_download_path.isEmpty()) {
            m_local_path = msg->_local_download_path;
        }
        else if (msg->_transfer_type == TransferType::Upload
            && !msg->_text_or_url.isEmpty()) {
            //发送方向：打开/另存为直接用本地源文件
            m_local_path = msg->_text_or_url;
        }
    }
    refreshButtons();
}

void FileBubble::setProgress(qint64 value, qint64 total_value)
{
    if (total_value <= 0) {
        return;
    }
    m_progressBar->show();
    m_progressBar->setValue(static_cast<int>(value * 100 / total_value));
    if (m_total_size != total_value) {
        m_total_size = total_value;
        m_sizeLabel->setText(humanSize(total_value));
    }
}

void FileBubble::setState(TransferState state)
{
    m_state = state;
    if (state == TransferState::Completed) {
        m_progressBar->hide();
    }
    refreshButtons();
}

void FileBubble::setDownloadFinish(std::shared_ptr<MsgInfo> msg, const QString& file_path)
{
    _msg_info = msg;
    m_local_path = file_path;
    m_state = TransferState::Completed;
    m_progressBar->hide();
    refreshButtons();
}

void FileBubble::setDownloadFailed(int error)
{
    Q_UNUSED(error);
    m_state = TransferState::Failed;
    m_progressBar->hide();
    refreshButtons();
}

void FileBubble::refreshButtons()
{
    //下载按钮：未开始/失败时可点；上传方向不显示下载
    const bool is_upload = (_msg_info && _msg_info->_transfer_type == TransferType::Upload);
    m_downloadBtn->setVisible(!is_upload && (m_state == TransferState::None
        || m_state == TransferState::Failed));
    m_pauseBtn->setVisible(m_state == TransferState::Downloading
        || m_state == TransferState::Uploading);
    m_resumeBtn->setVisible(m_state == TransferState::Paused
        || m_state == TransferState::Failed);
    m_openBtn->setEnabled(m_state == TransferState::Completed && !m_local_path.isEmpty());
    m_saveAsBtn->setEnabled(m_state == TransferState::Completed && !m_local_path.isEmpty());
}

void FileBubble::onOpenClicked()
{
    if (m_local_path.isEmpty() || !QFile::exists(m_local_path)) {
        QMessageBox::warning(this, QStringLiteral("打开"),
            QStringLiteral("文件不存在或已被清理"));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_local_path));
}

void FileBubble::onSaveAsClicked()
{
    if (m_local_path.isEmpty() || !QFile::exists(m_local_path)) {
        QMessageBox::warning(this, QStringLiteral("另存为"),
            QStringLiteral("文件不存在或已被清理"));
        return;
    }
    const QString dst = QFileDialog::getSaveFileName(this, QStringLiteral("另存为"),
        m_file_name);
    if (dst.isEmpty()) {
        return;
    }
    if (QFile::exists(dst)) {
        QFile::remove(dst);
    }
    if (!QFile::copy(m_local_path, dst)) {
        QMessageBox::warning(this, QStringLiteral("另存为"), QStringLiteral("保存失败"));
    }
}

QString FileBubble::humanSize(qint64 bytes)
{
    if (bytes >= 1024 * 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 2) + " GB";
    }
    if (bytes >= 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024), 'f', 2) + " MB";
    }
    if (bytes >= 1024) {
        return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    }
    return QString::number(bytes) + " B";
}
