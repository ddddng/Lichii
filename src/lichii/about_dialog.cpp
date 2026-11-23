// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <fmt/ranges.h>
#include "common/scm_rev.h"
#include "lichii/about_dialog.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace {

QLabel* AboutLabel(QWidget* parent, const QString& name, int size, int weight) {
    auto* label = new QLabel(parent);
    label->setObjectName(name);
    label->setFont(Spotlight::Font(size, weight));
    label->setWordWrap(true);
    return label;
}

} // namespace

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint) {
    static const std::string build_id = std::string{Common::g_build_id};
    // Published builds show only their tag (build-N), so add the commit a report came from.
    static const std::string version =
        Common::g_is_dev_build
            ? std::string{Common::g_build_version}
            : fmt::format("{} ({:.10})", Common::g_build_version, Common::g_scm_rev);
    static const std::string lichii_build =
        fmt::format("{} | {} | {}", std::string{Common::g_build_name}, version,
                    std::string{Common::g_compiler_id});

    const auto override_build =
        fmt::format(fmt::runtime(std::string(Common::g_title_bar_format_idle)), build_id);
    const auto lichii_build_version = override_build.empty() ? lichii_build : override_build;

    setObjectName(QStringLiteral("AboutDialog"));
    setWindowTitle(tr("About Lichii"));
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 24, 24);
    layout->setSpacing(0);

    auto* header = new QHBoxLayout;
    header->setSpacing(18);
    auto* logo = new QLabel(this);
    logo->setPixmap(QIcon::fromTheme(QStringLiteral("io.github.zortos293.lichii"),
                                     QIcon(QStringLiteral(":/img/lichii.ico")))
                        .pixmap(72));
    header->addWidget(logo, 0, Qt::AlignTop);
    auto* identity = new QVBoxLayout;
    identity->setSpacing(4);
    auto* name = new QLabel(QStringLiteral("Lichii"), this);
    name->setFont(Spotlight::DisplayFont(30));
    identity->addWidget(name);
    auto* build = AboutLabel(this, QStringLiteral("labelBuildInfo"), 12, QFont::Medium);
    build->setTextFormat(Qt::PlainText);
    build->setTextInteractionFlags(Qt::TextSelectableByMouse);
    build->setText(QStringLiteral("%1 (%2)").arg(QString::fromStdString(lichii_build_version),
                                                 QString::fromUtf8(Common::g_build_date).left(10)));
    identity->addWidget(build);
    header->addLayout(identity, 1);
    auto* close = new Spotlight::CircleButton(QStringLiteral("x"), 32, this);
    close->setAccessibleName(tr("Close"));
    close->setToolTip(tr("Close"));
    connect(close, &QAbstractButton::clicked, this, &QDialog::accept);
    header->addWidget(close, 0, Qt::AlignTop);
    layout->addLayout(header);
    layout->addSpacing(20);

    auto* about = AboutLabel(this, QStringLiteral("labelAbout"), 13, QFont::Normal);
    about->setText(tr("Lichii is an experimental open-source emulator for the Nintendo Switch 2, "
                      "based on Eglantine and Eden.\n\nThis software should not be used to play "
                      "games you have not legally obtained."));
    layout->addWidget(about);
    layout->addSpacing(18);

    auto* links = new QHBoxLayout;
    links->setSpacing(8);
    const auto add_link = [this, links](const QString& text, const QString& icon,
                                        const QString& url) {
        auto* link = new Spotlight::PillButton(text, icon, Spotlight::PillButton::Kind::Control,
                                               34, this);
        link->setToolTip(url);
        connect(link, &QAbstractButton::clicked, this,
                [url] { QDesktopServices::openUrl(QUrl(url)); });
        links->addWidget(link);
    };
    add_link(tr("Source Code"), QStringLiteral("arrow-square-out"),
             QStringLiteral("https://github.com/zortos293/Eglantine"));
    add_link(tr("Report an Issue"), QStringLiteral("bug"),
             QStringLiteral("https://github.com/zortos293/Eglantine/issues"));
    add_link(tr("Credits and License"), QStringLiteral("info"),
             QStringLiteral("https://github.com/zortos293/Eglantine#credits-and-license"));
    links->addStretch(1);
    layout->addLayout(links);
    layout->addSpacing(20);

    auto* rule = new QWidget(this);
    rule->setObjectName(QStringLiteral("aboutRule"));
    rule->setFixedHeight(1);
    layout->addWidget(rule);
    layout->addSpacing(14);

    auto* footer = new QHBoxLayout;
    footer->setSpacing(16);
    auto* liability = AboutLabel(this, QStringLiteral("labelLiability"), 11, QFont::Normal);
    liability->setText(tr("\"Nintendo Switch\" is a trademark of Nintendo. Lichii is not "
                          "affiliated with Nintendo in any way."));
    footer->addWidget(liability, 1);
    auto* done = new Spotlight::PillButton(tr("OK"), {}, Spotlight::PillButton::Kind::Accent, 34,
                                           this);
    connect(done, &QAbstractButton::clicked, this, &QDialog::accept);
    footer->addWidget(done, 0, Qt::AlignBottom);
    layout->addLayout(footer);
    done->setFocus();
}

void AboutDialog::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath card;
    card.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Spotlight::Radius::Card,
                        Spotlight::Radius::Card);
    painter.fillPath(card, Spotlight::Color::Panel());
    painter.setPen(QPen(Spotlight::Color::Stroke(), 1));
    painter.drawPath(card);
}

void AboutDialog::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && windowHandle() != nullptr) {
        windowHandle()->startSystemMove();
        return;
    }
    QDialog::mousePressEvent(event);
}
