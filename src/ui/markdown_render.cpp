#include "markdown_render.hpp"

#include <QByteArray>
#include "md4c-html.h"

namespace {
    void appendHtml(const MD_CHAR* data, MD_SIZE size, void* userData) {
        auto* buffer = static_cast<QByteArray*>(userData);
        buffer->append(data, static_cast<int>(size));
    }
}

QString markdownToHtml(const QString& text) {
    const QByteArray input = text.toUtf8();
    QByteArray output;

    const int result = md_html(
        input.constData(),
        static_cast<MD_SIZE>(input.size()),
        appendHtml,
        &output,
        MD_DIALECT_GITHUB | MD_FLAG_NOHTML,
        0
    );

    if (result != 0) {
        return text.toHtmlEscaped().replace("\n", "<br>");
    }

    return QString::fromUtf8(output);
}
