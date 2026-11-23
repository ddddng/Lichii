// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QDialog>
#include <QElapsedTimer>
#include <QImage>
#include <QPixmap>
#include <QWidget>

#include "common/common_types.h"
#include "core/file_sys/nup_writer.h"

class IconSearch;
class QAbstractButton;
class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QVBoxLayout;

namespace Spotlight {
class PillButton;
class SegmentedControl;
class ToggleSwitch;
} // namespace Spotlight

// Square preview that accepts an image dropped on it or chosen with a click.
class IconDropZone : public QWidget {
    Q_OBJECT
public:
    explicit IconDropZone(int size, QWidget* parent = nullptr);

    void SetIcon(const QPixmap& icon);

signals:
    void FileDropped(const QString& path);
    void Clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QPixmap icon;
    bool dragging = false;
    bool hovered = false;
};

// The square part of a picture to keep, chosen by the user; nothing when cancelled.
std::optional<QRect> ChooseIconCrop(const QImage& image, QWidget* parent);
// The square of a picture as a size x size JPEG, the way the console stores icons.
QByteArray EncodeArchiveIcon(const QImage& source, const QRect& crop, int size = 256);

// The NUP and NX dialogs: a rounded Spotlight card over the dimmed window, with a header that
// shows the game's icon, titled sections and Cancel and action pills.
class ArchiveCardDialog : public QDialog {
    Q_OBJECT
public:
    ArchiveCardDialog(const QString& title, int width, QWidget* parent);

protected:
    // Header: the icon (a drop zone or a picture), the title, a subtitle and a row of small pills
    // under them. Returns the subtitle.
    QLabel* SetHeader(QWidget* icon, const QString& subtitle,
                      const std::vector<QAbstractButton*>& pills = {});
    // A rounded section with a small title; returns its layout.
    QVBoxLayout* AddSection(const QString& title);
    // A label and a field side by side in a section.
    static void AddRow(QVBoxLayout* section, const QString& label, QWidget* field);
    // A muted explanation line.
    static QLabel* Hint(const QString& text);
    Spotlight::PillButton* AddButtons(const QString& action, const QString& action_icon);
    static Spotlight::PillButton* SmallPill(const QString& text, const QString& icon);

    void showEvent(QShowEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

    QWidget* card;
    QVBoxLayout* body;
    QLabel* title_label;
    Spotlight::PillButton* action_button = nullptr;
};

// A rounded card with a message, optional checkboxes and a row of pills, for the questions of
// archive tools. exec() returns Accepted for an accent pill; Clicked() names the pill.
class ArchiveMessageDialog : public ArchiveCardDialog {
    Q_OBJECT
public:
    ArchiveMessageDialog(const QString& title, const QString& text, QWidget* parent);

    QCheckBox* AddCheckBox(const QString& text, bool checked);
    QAbstractButton* AddButton(const QString& text, const QString& icon, bool accent);
    QAbstractButton* Clicked() const {
        return clicked;
    }

private:
    QVBoxLayout* checks = nullptr;
    QHBoxLayout* buttons = nullptr;
    QAbstractButton* clicked = nullptr;
};

// A rounded card with a message and an OK pill, for the results and errors of archive tools.
void ArchiveNotice(QWidget* parent, const QString& title, const QString& text);

// What Convert to NUP shows: the source, the packages it holds and what the NUP gets.
struct NupConvertSummary {
    QString source_name;
    std::filesystem::path archive;
    FileSys::Nup::Metadata detected;
    struct Package {
        QString label;
        QString detail;
        u64 bytes{};
        // Whether the user may leave it out.
        bool optional{};
    };
    std::vector<Package> packages;
    // Bytes needed besides the packages, such as the overlap of the temporary dump.
    u64 extra_bytes{};
    // A loose dump can be deleted while it is packed.
    bool offer_low_storage{};
};

class NupConvertDialog : public ArchiveCardDialog {
    Q_OBJECT
public:
    explicit NupConvertDialog(NupConvertSummary summary, QWidget* parent = nullptr);
    ~NupConvertDialog() override;

    // Only what differs from the detected metadata is an override.
    FileSys::Nup::Metadata Metadata() const;
    bool DeleteSources() const;
    std::vector<bool> IncludedPackages() const;
    std::filesystem::path Archive() const;
    QString DisplayName() const;
    QPixmap CurrentIcon() const;

private:
    void ChooseIcon();
    void SetIconFromFile(const QString& path);
    void ResetIcon();
    void UpdateIconPreview();
    void ChooseTarget();
    void UpdateSpace();

    NupConvertSummary summary;
    QByteArray custom_icon;
    QPixmap current_icon;
    std::filesystem::path archive;

    IconDropZone* icon_preview;
    Spotlight::PillButton* reset_icon;
    QLineEdit* name_edit;
    QLineEdit* publisher_edit;
    QLineEdit* version_edit;
    std::vector<Spotlight::ToggleSwitch*> package_toggles;
    QLabel* target_label;
    QLabel* space_label;
    Spotlight::ToggleSwitch* low_storage = nullptr;
};

// Edits the title, publisher, version and icon stored in a NUP or NX archive.
class ArchiveEditDialog : public ArchiveCardDialog {
    Q_OBJECT
public:
    enum class Format { Nup, Nx };

    // original_icon: the icon the archive was made with, offered for restoring (NX).
    ArchiveEditDialog(Format format, const std::filesystem::path& archive,
                      FileSys::Nup::Metadata current, std::vector<u8> original_icon = {},
                      QWidget* parent = nullptr);
    ~ArchiveEditDialog() override;

    // nullopt fields keep what is stored. An NX archive takes no version or localized names.
    FileSys::Nup::MetadataEdit Edit() const;
    QString DisplayName() const;
    QPixmap CurrentIcon() const;

private:
    void ChooseIcon();
    void UseImage(const QImage& image, bool crop);
    void SetIconFromFile(const QString& path);
    void FindIcon();
    void OnIconFound(const QByteArray& png, const QString& resolved_title);
    void ClearIcon();
    void RestoreIcon();
    void EncodeIcon();
    void UpdateIconPreview();

    Format format;
    FileSys::Nup::Metadata current;
    std::vector<u8> original_icon;
    QImage icon_source;
    QRect icon_crop;
    QByteArray custom_icon;
    bool remove_icon = false;
    QPixmap current_icon;

    IconDropZone* icon_preview;
    QLabel* icon_hint;
    Spotlight::PillButton* find_icon;
    IconSearch* icon_search;
    QLineEdit* name_edit;
    QLineEdit* publisher_edit;
    QLineEdit* version_edit = nullptr;
    Spotlight::SegmentedControl* icon_size = nullptr;
    QWidget* localized_panel = nullptr;
    std::vector<std::pair<std::string, QLineEdit*>> localized_edits;
};

// Shown while a conversion or an edit runs: the game, the step, a bar, how much is done and a
// rough time remaining.
class NupProgressDialog : public ArchiveCardDialog {
    Q_OBJECT
public:
    NupProgressDialog(const QString& operation, const QString& title, const QPixmap& icon,
                      QWidget* parent = nullptr);

    // A step of a job with several, such as "Step 1 of 2 · Reading the game". Restarts the
    // time estimate.
    void SetStep(const QString& step);
    void SetProgress(const FileSys::Nup::PackProgress& progress);
    void SetCancelling();
    // Closes the dialog without treating it as a cancel request.
    void Finish();

signals:
    void CancelRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    QLabel* status;
    QLabel* details;
    QProgressBar* bar;
    Spotlight::PillButton* cancel;
    QString step;
    QElapsedTimer timer;
    bool cancelling = false;
    bool finished = false;
};
