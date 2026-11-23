// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScrollArea>
#include <QVBoxLayout>
#include "dep_hashes.h"
#include "lichii/deps_dialog.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace {

constexpr int VersionWidth = 118;

QWidget* Divider(QWidget* parent) {
    auto* line = new QWidget(parent);
    line->setFixedHeight(1);
    line->setAttribute(Qt::WA_StyledBackground);
    line->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 18);"));
    return line;
}

} // namespace

DepsDialog::DepsDialog(QWidget* parent) : Spotlight::Dialog(parent) {
    setWindowTitle(tr("Lichii Dependencies"));
    SetCardWidth(509);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(14);

    auto* intro = new QHBoxLayout;
    intro->setSpacing(12);
    auto* logo = new QLabel(this);
    const qreal ratio = devicePixelRatioF();
    QPixmap icon(qRound(42 * ratio), qRound(42 * ratio));
    icon.setDevicePixelRatio(ratio);
    icon.fill(Qt::transparent);
    {
        QPainter painter(&icon);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        Spotlight::PaintWholeIcon(painter, QRectF(0, 0, 42, 42),
                                  QPixmap(QStringLiteral(":/icons/default/256x256/lichii.png")), 11);
    }
    logo->setPixmap(icon);
    intro->addWidget(logo);
    auto* info = new QLabel(tr("The projects that make Lichii possible"), this);
    info->setFont(Spotlight::Font(13, 400));
    Spotlight::SetTextColor(info, Spotlight::Color::TextSecondary());
    intro->addWidget(info, 1);
    layout->addLayout(intro);

    auto* table = new QWidget;
    table->setObjectName(QStringLiteral("depsTable"));
    auto* rows = new QVBoxLayout(table);
    rows->setContentsMargins(12, 12, 12, 12);
    rows->setSpacing(9);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPixelSize(13);
    const auto add_row = [&](QWidget* name, const QString& version, const QFont& font,
                             const QColor& color) {
        auto* row = new QHBoxLayout;
        row->setSpacing(12);
        row->addWidget(name);
        row->addStretch(1);
        auto* value = new QLabel(table);
        value->setFont(font);
        value->setFixedWidth(VersionWidth);
        value->setText(QFontMetrics(font).elidedText(version, Qt::ElideRight, VersionWidth));
        if (value->text() != version) {
            value->setToolTip(version);
        }
        Spotlight::SetTextColor(value, color);
        row->addWidget(value);
        rows->addLayout(row);
    };
    auto* header = new QLabel(tr("Dependency"), table);
    header->setFont(Spotlight::Font(13, 600));
    add_row(header, tr("Version"), Spotlight::Font(13, 600), Spotlight::Color::Text());
    const QPixmap link = Spotlight::IconPixmap(u"arrow-square-out", 14, Spotlight::Color::Info(),
                                               devicePixelRatioF());
    for (std::size_t i = 0; i < Common::dep_names.size(); ++i) {
        auto* divider_row = new QHBoxLayout;
        divider_row->setContentsMargins(0, 0, 18, 0);
        divider_row->addWidget(Divider(table));
        rows->addLayout(divider_row);

        auto* name = new QWidget(table);
        auto* name_layout = new QHBoxLayout(name);
        name_layout->setContentsMargins(0, 0, 0, 0);
        name_layout->setSpacing(6);
        auto* text = new QLabel(QStringLiteral("<a href=\"%1\" style=\"color: #8fb5ff; "
                                               "text-decoration: none\">%2</a>")
                                    .arg(QString::fromUtf8(Common::dep_urls[i]),
                                         QString::fromUtf8(Common::dep_names[i]).toHtmlEscaped()),
                                name);
        text->setFont(Spotlight::Font(13, 600));
        text->setTextInteractionFlags(Qt::TextBrowserInteraction);
        text->setOpenExternalLinks(true);
        name_layout->addWidget(text);
        auto* arrow = new QLabel(name);
        arrow->setPixmap(link);
        name_layout->addWidget(arrow);
        add_row(name, QString::fromUtf8(Common::dep_hashes[i]), mono,
                Spotlight::Color::TextSecondary());
    }

    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("depsCard"));
    scroll->setWidget(table);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "#depsCard { background-color: rgba(255, 255, 255, 13); "
        "border: 1px solid rgba(255, 255, 255, 20); border-radius: 18px; } "
        "#depsTable { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setFixedHeight(std::min(table->sizeHint().height() + 2, 340));
    layout->addWidget(scroll);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, this);
    buttons->setContentsMargins(0, 12, 0, 0);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}

DepsDialog::~DepsDialog() = default;
