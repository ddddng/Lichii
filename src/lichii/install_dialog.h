// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "lichii/spotlight/dialog.h"

class QCheckBox;
class QDialogButtonBox;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QVBoxLayout;

class InstallDialog : public Spotlight::Dialog {
    Q_OBJECT

public:
    explicit InstallDialog(QWidget* parent, const QStringList& files);
    ~InstallDialog() override;

    [[nodiscard]] QStringList GetFiles() const;
    [[nodiscard]] int GetMinimumWidth() const;

private:
    QListWidget* file_list;

    QVBoxLayout* vbox_layout;
    QHBoxLayout* hbox_layout;

    QLabel* description;
    QLabel* update_description;
    QDialogButtonBox* buttons;
};
