// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QIcon>
#include <QMenu>

class QApplication;

namespace Spotlight {

/// The menu panel of the design (context-menu.jsx): 36 px rows, a 16 px icon column, shortcut
/// hints and submenu carets on the right. A "\t" in an action's text separates its hint.
class Menu : public QMenu {
public:
    explicit Menu(QWidget* parent = nullptr);

    /// Adds a submenu painted the same way.
    Menu* AddSubmenu(const QIcon& icon, const QString& title);

    /// Adds a menu that also lives elsewhere, such as in the menu bar, with an icon only this menu
    /// shows.
    void AddSharedMenu(QMenu* menu, const QIcon& icon);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QHash<const QAction*, QIcon> shared_icons;
};

/// Gives every menu of the application a transparent window so its rounded panel has no
/// square corners.
void InstallMenuStyle(QApplication& application);

} // namespace Spotlight
