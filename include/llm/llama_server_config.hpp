#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

struct LlamaServerConfig {
    QString executablePath;
    QStringList arguments;
    QUrl healthUrl;
    int startupTimeoutMs = 120000;
};
