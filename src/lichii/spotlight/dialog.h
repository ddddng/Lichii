// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDialog>

namespace Spotlight {

/// A dialog in Spotlight's floating card (Paper: dialog card, radius 26) with a soft shadow, a
/// title row (the window title, an optional subtitle and a close button) and the dialog's own
/// layout below it; the style sheet turns its QDialogButtonBox into pill buttons. Derive from it
/// instead of QDialog to adopt it: setupUi(this) and layouts set on the dialog work unchanged and
/// sit inside the card. The card moves with the pointer anywhere outside its controls.
class Dialog : public QDialog {
    Q_OBJECT
public:
    explicit Dialog(QWidget* parent = nullptr, Qt::WindowFlags flags = {});

    /// A muted line under the title, such as the game a dialog is about. Empty hides it.
    void SetSubtitle(const QString& subtitle);

    /// Hides the title, for a dialog whose contents start with their own heading. The close button
    /// stays.
    void SetTitleVisible(bool visible);
};

/// Puts a dialog that cannot derive from Dialog in the same card, without a close button when
/// `close_button` is false. Call it before the dialog is first shown; a dialog whose window
/// already exists keeps its frame. The Spotlight style does this for every QMessageBox,
/// QInputDialog and QProgressDialog.
void AdoptDialogChrome(QDialog* dialog, bool close_button = true);

} // namespace Spotlight
