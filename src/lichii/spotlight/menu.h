// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QIcon>
#include <QMenu>

class QApplication;

namespace Spotlight {

/// A menu for the menu panel of the design (context-menu.jsx). InstallMenuStyle paints every menu
/// that way: 36 px rows, a 16 px icon column, shortcut hints and submenu carets on the right. A
/// "\t" in an action's text separates its hint, and a "spotlightHint"
/// property replaces it.
class Menu : public QMenu {
public:
    explicit Menu(QWidget* parent = nullptr);

    /// Adds a submenu.
    Menu* AddSubmenu(const QIcon& icon, const QString& title);

    /// Adds a menu that also lives elsewhere, such as in the menu bar, with an icon only the menu
    /// panels show.
    void AddSharedMenu(QMenu* menu, const QIcon& icon);
};

/// Paints every menu of the application as the design's panel, sized to fit its labels, in a
/// transparent window so the rounded panel has no square corners.
void InstallMenuStyle(QApplication& application);

} // namespace Spotlight
