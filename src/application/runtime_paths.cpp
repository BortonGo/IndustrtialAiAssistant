#include "runtime_paths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

QString findRuntimeRoot()
{
    QDir directory(QCoreApplication::applicationDirPath());
    for (int depth = 0; depth < 5; ++depth) {
        if (QFileInfo(directory.filePath("models")).isDir() &&
            QFileInfo(directory.filePath("tools/llama-cpp")).isDir()) {
            return directory.absolutePath();
        }
        if (!directory.cdUp()) {
            break;
        }
    }
    return QCoreApplication::applicationDirPath();
}
