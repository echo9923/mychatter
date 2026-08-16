#include "global.h"
#include <QEventLoop>
#include <QTimer>
#include <QUuid>
#include <QPainter>
#include <QFileInfo>
#include <QVector>

std::function<void(QWidget*)> repolish =[](QWidget *w){
    w->style()->unpolish(w);
    w->style()->polish(w);
};

std::function<QString(QString)> xorString = [](QString input){
    QString result = input; // 复制原始字符串，以便进行修改
    int length = input.length(); // 获取字符串的长度
    ushort xor_code = length % 255;
    for (int i = 0; i < length; ++i) {
        // 对每个字符进行异或操作
        // 注意：这里假设字符都是ASCII，因此直接转换为QChar
        result[i] = QChar(static_cast<ushort>(input[i].unicode() ^ xor_code));
    }
    return result;
};

QString gate_url_prefix = "";

QString generateUniqueFileName(const QString& originalName){

     QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
     QFileInfo fileInfo(originalName);
     QString extension = fileInfo.suffix();
     return uuid + (extension.isEmpty() ? "" : "." + extension);
}

QString generateUniqueIconName(){
    QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return uuid + ".png";
}

QString calculateFileSha256Only(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return QString();

    QCryptographicHash hash(QCryptographicHash::Sha256);

    // 分块计算哈希，避免大文件占用过多内存
    const qint64 chunkSize = 1024 * 1024; // 1MB
    while (!file.atEnd())
    {
        hash.addData(file.read(chunkSize));
    }
    file.close();

    return QString::fromLatin1(hash.result().toHex().toLower());
}

bool calculateFileSha256(const QString& filePath, QString& content_hash,
    QVector<QString>& chunk_hashes)
{
    content_hash.clear();
    chunk_hashes.clear();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    //一次 32KiB 遍历同时产出整文件哈希与每片哈希：
    //整文件用一个持续 addData 的对象；每片另起临时对象，与上传分片粒度一致
    QCryptographicHash whole(QCryptographicHash::Sha256);
    while (!file.atEnd())
    {
        const QByteArray chunk = file.read(MAX_FILE_LEN);
        if (chunk.isEmpty()) {
            break;
        }
        whole.addData(chunk);
        QCryptographicHash chunkHash(QCryptographicHash::Sha256);
        chunkHash.addData(chunk);
        chunk_hashes.push_back(QString::fromLatin1(chunkHash.result().toHex().toLower()));
    }
    file.close();

    content_hash = QString::fromLatin1(whole.result().toHex().toLower());
    return !content_hash.isEmpty();
}

QString guessMimeType(const QString& fileName)
{
    //常见扩展名到 MIME 的映射；未命中回退 application/octet-stream
    static const struct { const char* ext; const char* mime; } kMap[] = {
        { "png",  "image/png" },
        { "jpg",  "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif",  "image/gif" },
        { "bmp",  "image/bmp" },
        { "webp", "image/webp" },
        { "svg",  "image/svg+xml" },
        { "pdf",  "application/pdf" },
        { "zip",  "application/zip" },
        { "rar",  "application/vnd.rar" },
        { "7z",   "application/x-7z-compressed" },
        { "txt",  "text/plain" },
        { "md",   "text/markdown" },
        { "doc",  "application/msword" },
        { "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
        { "xls",  "application/vnd.ms-excel" },
        { "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
        { "ppt",  "application/vnd.ms-powerpoint" },
        { "pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
        { "mp3",  "audio/mpeg" },
        { "mp4",  "video/mp4" },
        { "csv",  "text/csv" },
    };
    const QString ext = QFileInfo(fileName).suffix().toLower();
    for (const auto& item : kMap) {
        if (ext == QLatin1String(item.ext)) {
            return QLatin1String(item.mime);
        }
    }
    return QStringLiteral("application/octet-stream");
}

QPixmap CreateLoadingPlaceholder(int width, int height ) {
    QPixmap placeholder(width, height);
    placeholder.fill(QColor(240, 240, 240)); // 浅灰色背景

    QPainter painter(&placeholder);
    painter.setRenderHint(QPainter::Antialiasing);

    // 绘制边框
    painter.setPen(QPen(QColor(200, 200, 200), 2));
    painter.drawRect(1, 1, width - 2, height - 2);

    // 绘制加载图标（简单的旋转圆圈或文字）
    QFont font;
    font.setPointSize(12);
    painter.setFont(font);
    painter.setPen(QColor(150, 150, 150));
    painter.drawText(placeholder.rect(), Qt::AlignCenter, "加载中...");

    // 可选：添加图片图标
    painter.setPen(QColor(180, 180, 180));
    QRect iconRect(width / 2 - 20, height / 2 - 40, 40, 30);
    painter.drawRect(iconRect);
    painter.drawLine(iconRect.topLeft(), iconRect.bottomRight());
    painter.drawLine(iconRect.topRight(), iconRect.bottomLeft());

    return placeholder;
}