// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <QKeySequence>
#include <QPixmap>
#include <QWidget>

class QAction;
class QLabel;
class QMenu;
class QScrollArea;

namespace Spotlight {

class GameArt;
class SegmentedControl;
class ValueButton;
class VolumeControl;

/// The menus behind the in-game dropdowns; each choice applies at once, as in the View menu.
struct InGameControls {
    QMenu* renderer;
    QMenu* filter;
    QMenu* frame_gen;
    QMenu* more;
    QAction* settings;
    QAction* decrease_volume;
    QAction* increase_volume;
};

/// What the in-game panels show, refreshed by MainWindow.
struct InGameSummary {
    QString renderer;
    QString resolution;
    QString filter;
    QString frame_gen;
    QString speed;
    QString speed_keys;
    int preset = -1;
    bool preset_global = true;
};

/// The bottom bar's "Vulkan · Docked" popover.
class QuickControls final : public QWidget {
    Q_OBJECT

public:
    QuickControls(const InGameControls& controls, QWidget* parent);
    void Open(QWidget* anchor, QWidget* bar);
    void SetSummary(const InGameSummary& summary);
    void SetFrameTime(const QString& text);
    void SetConsoleMode(bool docked);
    void SetVolume(int volume, bool muted);

signals:
    void AboutToOpen();
    void Dismissed();
    void DockedChanged(bool docked);
    void VolumeChanged(int volume);

protected:
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent* event) override;

private:
    ValueButton* renderer;
    ValueButton* resolution;
    ValueButton* filter;
    ValueButton* frame_gen;
    QLabel* speed;
    QLabel* speed_keys;
    QLabel* frame_time;
    SegmentedControl* console_mode;
    VolumeControl* volume;
};

class InGameMenu final : public QWidget {
    Q_OBJECT

public:
    struct Actions {
        QAction* pause;
        QAction* restart;
        QAction* stop;
        QAction* capture;
        QAction* fullscreen;
        QAction* frame_gen;
        QAction* turbo;
        QAction* effects;
        InGameControls controls;
    };

    explicit InGameMenu(const Actions& actions, QWidget* parent);
    void Open(QWidget* anchor, const QKeySequence& shortcut, const QPixmap& icon,
              const QString& title, const QString& play_time, bool paused);
    void SetSummary(const InGameSummary& summary);
    void SetConsoleMode(bool docked);
    void SetVolume(int volume, bool muted);

signals:
    void Dismissed();
    void DockedChanged(bool docked);
    void VolumeChanged(int volume);

protected:
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    bool IsCloseKey(const QKeyEvent& event) const;
    QAction* ActionForKey(const QKeyEvent& event) const;

    QKeySequence menu_shortcut;
    std::array<QAction*, 6> popup_actions;
    QWidget* close_hint;
    GameArt* art;
    QWidget* panel;
    QScrollArea* scroll;
    QWidget* content;
    QLabel* title;
    QLabel* play_time;
    QWidget* pause_tile;
    QLabel* preset_note;
    SegmentedControl* preset;
    ValueButton* resolution;
    ValueButton* frame_gen;
    SegmentedControl* console_mode;
    VolumeControl* volume;
};

} // namespace Spotlight
