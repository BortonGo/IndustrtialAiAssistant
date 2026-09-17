#include "document_loader.hpp"

#include <QFile>
#include <QFileInfo>
#include <QByteArray>

#include <stdexcept>
#include <utility>

DocumentLoader::DocumentLoader(qint64 maxFileSize) : maxFileSize_(maxFileSize) {
    if (maxFileSize <= 0) {
        throw std::invalid_argument("Maximum document size must be > 0");
    }
}

Document DocumentLoader::loadTxtFile(const QString& path) const {

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(file.errorString().toUtf8().constData());
    }
    if (file.size() > maxFileSize_) {
        throw std::runtime_error("File exceeds size limit");
    }

    auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        throw std::runtime_error(file.errorString().toUtf8().constData());
    }

    QString text = QString::fromUtf8(bytes);
    if (text.trimmed().isEmpty()) {
        throw std::runtime_error("File without text");
    }

    const QFileInfo fileInfo(path);

    Document document{};
    document.id = fileInfo.absoluteFilePath();
    document.sourcePath = fileInfo.absoluteFilePath();
    document.text = std::move(text);

    return document;
}
