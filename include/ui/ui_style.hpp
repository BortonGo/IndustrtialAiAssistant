#pragma once

#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QString>

// Shared graphite palette and small navigation icons; the application emblem
// continues to come from resources/icons/logo-dark.svg.
namespace UiStyle {
inline QString applicationStyleSheet() {
    return QStringLiteral(
        "QWidget { color: #ECECEE; font-family: 'Segoe UI'; font-size: 14px; }"
        "QMainWindow, QWidget#mainSurface, QWidget#workspace { background: #191A1B; }"
        "QWidget#workspaceHeader { background: #191A1B; border-bottom: 1px solid #363739; }"
        "QPushButton, QToolButton { background: transparent; color: #A5A6AA; border: none;"
        " border-radius: 7px; padding: 7px 10px; }"
        "QToolButton { padding: 5px; }"
        "QPushButton:hover, QToolButton:hover { background: #2D2E30; color: #ECECEE; }"
        "QPushButton:checked, QToolButton:checked { background: #323437; color: #ECECEE; }"
        "QPushButton:disabled, QToolButton:disabled { color: #84858A; }"
        "QPushButton:focus, QToolButton:focus { border: 1px solid #CCD7E8; }"
        "QToolTip { background: #242527; color: #ECECEE; border: 1px solid #363739; padding: 5px; }"
        "QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }"
        "QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; }"
        "QScrollBar::handle { background: #454649; border-radius: 4px; }"
        "QScrollBar::handle:vertical { min-height: 30px; }"
        "QScrollBar::handle:horizontal { min-width: 30px; }"
        "QScrollBar::handle:hover { background: #626367; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }"
        "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }"
        "QMessageBox, QDialog { background: #202122; }"
    );
}

enum class Icon { NewChat, Documents, Sidebar, Sources, Plus, Send, Back, Close, Copy };

inline QIcon icon(Icon kind, const QColor& color = QColor("#A5A6AA")) {
    QPixmap pixels(40, 40); pixels.fill(Qt::transparent);
    QPainter painter(&pixels); painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(2, 2);
    painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (kind) {
    case Icon::Sidebar:
    case Icon::Sources:
        painter.drawRoundedRect(QRectF(3, 3.5, 14, 13), 1.5, 1.5);
        painter.drawLine(kind == Icon::Sidebar ? 8 : 12, 4, kind == Icon::Sidebar ? 8 : 12, 16);
        break;
    case Icon::NewChat: {
        QPainterPath outline; outline.moveTo(9,4); outline.lineTo(4,4); outline.lineTo(4,16);
        outline.lineTo(16,16); outline.lineTo(16,11); painter.drawPath(outline);
        QPolygonF pen; pen << QPointF(8,12) << QPointF(9,8) << QPointF(15,2)
                          << QPointF(18,5) << QPointF(12,11) << QPointF(8,12);
        painter.drawPolyline(pen); break;
    }
    case Icon::Documents:
        painter.drawRoundedRect(QRectF(6, 5, 10, 12), 1.5, 1.5);
        painter.drawLine(3, 3, 12, 3); painter.drawLine(3, 3, 3, 14);
        painter.drawLine(9, 9, 13, 9); painter.drawLine(9, 12, 13, 12); break;
    case Icon::Plus: painter.drawLine(4, 10, 16, 10); painter.drawLine(10, 4, 10, 16); break;
    case Icon::Send:
        painter.drawLine(10, 16, 10, 4); painter.drawLine(5, 9, 10, 4); painter.drawLine(10, 4, 15, 9); break;
    case Icon::Back:
        painter.drawLine(16, 10, 4, 10); painter.drawLine(9, 5, 4, 10); painter.drawLine(4, 10, 9, 15); break;
    case Icon::Close: painter.drawLine(5, 5, 15, 15); painter.drawLine(5, 15, 15, 5); break;
    case Icon::Copy:
        painter.drawRoundedRect(QRectF(7, 6, 10, 11), 1.5, 1.5);
        painter.drawLine(3, 13, 3, 3); painter.drawLine(3, 3, 12, 3); break;
    }
    return QIcon(pixels);
}
}
