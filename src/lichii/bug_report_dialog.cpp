// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "lichii/bug_report_dialog.h"
#include "lichii/spotlight/notice.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

BugReportDialog::BugReportDialog(Core::System& system, QWidget* parent)
    : Spotlight::Dialog(parent), stop{std::make_shared<std::atomic<bool>>(false)},
      bundle{std::make_shared<Core::Nx2Debug::BugBundle>(
          system, Core::Nx2Debug::BugBundle::Options{.trigger = "ui"})} {
    setWindowTitle(tr("Report a Bug"));
    SetIcon(u"bug", Spotlight::Color::Accent());
    setFixedWidth(576);

    capture = QtConcurrent::run([bundle = bundle, stop = stop] {
        try {
            bundle->Capture(*stop);
            return QString{};
        } catch (const std::exception& failure) {
            return QString::fromStdString(failure.what());
        }
    });

    auto* const layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    auto* const intro = new QLabel(
        tr("Describe what went wrong and what you expected. Lichii saves the log, settings, a "
           "screenshot and the next frame with its shaders to a local ZIP."));
    intro->setWordWrap(true);
    intro->setFont(Spotlight::Font(13, 400));
    Spotlight::SetTextColor(intro, Spotlight::Color::TextSecondary());
    layout->addWidget(intro);

    description = new QPlainTextEdit;
    description->setPlaceholderText(
        tr("For example: the sky turns white after the first cutscene in world 2."));
    description->setFont(Spotlight::Font(13, 400));
    description->setMinimumHeight(80);
    layout->addWidget(description, 1);

    auto* const privacy = new Spotlight::Notice;
    privacy->setText(tr("Send the report privately to the Lichii maintainers (the log bot's /bug "
                        "command or a direct message), not to a public channel or issue. It "
                        "includes a frame's game textures and shaders. User names in paths and "
                        "online service settings are left out."));
    layout->addWidget(privacy);

    status = new Spotlight::Notice;
    status->hide();
    layout->addWidget(status);
    details = new QLabel;
    details->setWordWrap(true);
    details->setFont(Spotlight::Font(13, 400));
    Spotlight::SetTextColor(details, Spotlight::Color::TextSecondary());
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->hide();
    layout->addWidget(details);

    buttons = new QDialogButtonBox;
    save = buttons->addButton(tr("Save Report"), QDialogButtonBox::AcceptRole);
    save->setProperty("kind", QStringLiteral("primary"));
    buttons->addButton(QDialogButtonBox::Cancel);
    open_folder = new QPushButton(
        Spotlight::Icon(QStringLiteral("folder-open"), Spotlight::Color::Text(), 16),
        tr("Open Folder"));
    open_folder->setAutoDefault(false);
    open_folder->hide();
    auto* const button_row = new QHBoxLayout;
    button_row->setContentsMargins(0, 6, 0, 0);
    button_row->addWidget(open_folder);
    button_row->addWidget(buttons, 1);
    layout->addLayout(button_row);

    connect(save, &QPushButton::clicked, this, &BugReportDialog::Save);
    connect(buttons, &QDialogButtonBox::rejected, this, &BugReportDialog::reject);
    connect(open_folder, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(
            Core::Nx2Debug::BugBundle::DefaultDirectory().generic_string())));
    });
    connect(&writer, &QFutureWatcher<Written>::finished, this,
            [this] { ShowResult(writer.result()); });
}

BugReportDialog::~BugReportDialog() {
    StopCapture();
    writer.waitForFinished();
}

void BugReportDialog::StopCapture() {
    stop->store(true);
    capture.waitForFinished();
}

void BugReportDialog::Save() {
    writing = true;
    save->setEnabled(false);
    description->setReadOnly(true);
    buttons->button(QDialogButtonBox::Cancel)->setEnabled(false);
    status->SetTone(Spotlight::Notice::Tone::Neutral);
    status->setText(tr("Writing the report. The frame capture can take a minute in a busy scene."));
    status->show();
    writer.setFuture(QtConcurrent::run([bundle = bundle, capture = capture,
                                        text = description->toPlainText().toStdString()] {
        Written written;
        QFuture<QString> pending = capture;
        written.error = pending.result();
        if (!written.error.isEmpty()) {
            return written;
        }
        try {
            written.result = bundle->Write(text);
        } catch (const std::exception& failure) {
            written.error = QString::fromStdString(failure.what());
        }
        return written;
    }));
}

void BugReportDialog::ShowResult(const Written& written) {
    writing = false;
    auto* const close = buttons->button(QDialogButtonBox::Cancel);
    close->setEnabled(true);
    close->setText(tr("Close"));
    close->setProperty("kind", QStringLiteral("primary"));
    close->style()->unpolish(close);
    close->style()->polish(close);
    save->hide();
    if (!written.error.isEmpty()) {
        status->SetTone(Spotlight::Notice::Tone::Error);
        status->setText(tr("The report could not be written: %1").arg(written.error));
        return;
    }
    status->SetTone(Spotlight::Notice::Tone::Success);
    status->setText(tr("Saved %1 (%2 MiB).")
                        .arg(QString::fromStdString(written.result.path.generic_string()))
                        .arg(static_cast<double>(written.result.bytes) / (1 << 20), 0, 'f', 1));
    QStringList omissions;
    if (!written.result.frame_error.empty()) {
        omissions.append(tr("No frame capture: %1")
                             .arg(QString::fromStdString(written.result.frame_error)));
    }
    if (!written.result.dropped.empty()) {
        omissions.append(tr("Left out to fit the size limit: %1")
                             .arg(QString::number(written.result.dropped.size())));
    }
    details->setText(omissions.join(QStringLiteral("\n\n")));
    details->setHidden(omissions.isEmpty());
    open_folder->show();
}

void BugReportDialog::reject() {
    if (!writing) {
        QDialog::reject();
    }
}
