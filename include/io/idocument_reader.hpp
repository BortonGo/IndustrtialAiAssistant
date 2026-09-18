#pragma once

#include "documents/document.hpp"

#include <QObject>
#include <QString>

class IDocumentReader : public QObject {
    Q_OBJECT
public:
    explicit IDocumentReader(QObject* parent = nullptr);
    virtual void load(const QString& path) = 0;
    ~IDocumentReader() override = default;

signals:
    void documentReady(const Document& document);
    void errorOccurred(const QString& message);
};
