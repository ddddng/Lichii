// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/spotlight/notice.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>

#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace Spotlight {

namespace {

struct Palette {
    QColor tint;
    QColor fill;
    QColor stroke;
    QColor text;
    const char16_t* icon;
};

Palette ToneColors(Notice::Tone tone) {
    switch (tone) {
    case Notice::Tone::Success:
        return {QColor(0x7e, 0xe2, 0xa8), QColor(0x7e, 0xe2, 0xa8, 0x14),
                QColor(0x7e, 0xe2, 0xa8, 0x33), QColor(0xff, 0xff, 0xff, 0xb8), u"check-circle"};
    case Notice::Tone::Error:
        return {QColor(0xff, 0x8f, 0xae), QColor(0xff, 0x8f, 0xae, 0x1f),
                QColor(0xff, 0x8f, 0xae, 0x40), QColor(0xff, 0xc2, 0xd2), u"warning-circle"};
    case Notice::Tone::Warning:
        return {QColor(0xff, 0xd3, 0x7a), QColor(0xff, 0xd3, 0x7a, 0x14),
                QColor(0xff, 0xd3, 0x7a, 0x33), QColor(0xff, 0xe7, 0xb3), u"warning-circle"};
    case Notice::Tone::Neutral:
        break;
    }
    return {Color::TextSecondary(), QColor(0xff, 0xff, 0xff, 0x0a), Color::StrokeSubtle(),
            QColor(0xff, 0xff, 0xff, 0xb8), u"info"};
}

} // namespace

Notice::Notice(QWidget* parent) : QWidget(parent) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);
    icon = new QLabel(this);
    icon->setFixedSize(16, 18);
    icon->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon, 0, Qt::AlignTop);
    label = new QLabel(this);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setFont(Font(13, 400));
    layout->addWidget(label, 1);
    SetTone(Tone::Neutral);
}

void Notice::SetTone(Tone tone_) {
    tone = tone_;
    SetTextColor(label, ToneColors(tone).text);
    UpdateIcon();
    update();
}

void Notice::SetText(const QString& text) {
    label->setText(text);
}

QString Notice::Text() const {
    return label->text();
}

void Notice::SetOpenExternalLinks(bool open) {
    label->setTextFormat(open ? Qt::RichText : Qt::PlainText);
    label->setOpenExternalLinks(open);
    label->setTextInteractionFlags(open ? Qt::TextBrowserInteraction : Qt::NoTextInteraction);
}

void Notice::paintEvent(QPaintEvent*) {
    const auto colors = ToneColors(tone);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colors.stroke, 1));
    painter.setBrush(colors.fill);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
}

void Notice::changeEvent(QEvent* event) {
    if (event->type() == QEvent::DevicePixelRatioChange) {
        UpdateIcon();
    }
    QWidget::changeEvent(event);
}

void Notice::UpdateIcon() {
    const auto colors = ToneColors(tone);
    icon->setPixmap(IconPixmap(QStringView{colors.icon}, 16, colors.tint, devicePixelRatioF()));
}

} // namespace Spotlight
