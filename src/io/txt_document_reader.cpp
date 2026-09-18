#include "txt_document_reader.hpp"

#include <QFile>
#include <QFileInfo>
#include <QByteArray>

#include <stdexcept>
#include <utility>

TxtDocumentReader::TxtDocumentReader(qint64 maxFileSize, QObject* parent) :
    IDocumentReader(parent), maxFileSize_(maxFileSize) {}

Document TxtDocumentReader::loadFile(const QString& path) const {
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

void TxtDocumentReader::load(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit errorOccurred(file.errorString());
        return;
    }
    if (file.size() > maxFileSize_) {
        emit errorOccurred("File exceeds size limit");
        return;
    }

    auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        emit errorOccurred(file.errorString());
        return;
    }

    QString text = QString::fromUtf8(bytes);
    if (text.trimmed().isEmpty()) {
        emit errorOccurred("File without text");
        return;
    }

    const QFileInfo fileInfo(path);

    Document document{};
    document.id = fileInfo.absoluteFilePath();
    document.sourcePath = fileInfo.absoluteFilePath();
    document.text = std::move(text);

    emit documentReady(document);

    return;
}
