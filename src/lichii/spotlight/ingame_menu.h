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
    QMenu* accuracy;
    QMenu* anti_aliasing;
    /// Null in builds without post-processing.
    QMenu* effects;
    QAction* settings;
    QAction* decrease_volume;
    QAction* increase_volume;
    QAction* mute;
    QAction* reset_volume;
};

/// What the in-game panels show, refreshed by MainWindow.
struct InGameSummary {
    QString renderer;
    QString resolution;
    QString filter;
    QString frame_gen;
    QString accuracy;
    QString anti_aliasing;
    QString effects;
    /// 0 Auto, 1 Low, 2 Mid, 3 High; -1 when the values match no preset.
    int preset = -1;
    bool preset_global = true;
};

/// A panel that drops down from a running title bar menu button.
class InGamePopover : public QWidget {
    Q_OBJECT

public:
    explicit InGamePopover(QWidget* parent);
    void Open(QWidget* anchor);

signals:
    void AboutToOpen();
    void Dismissed();

protected:
    void paintEvent(QPaintEvent*) override;
    void hideEvent(QHideEvent* event) override;
};

/// Graphics: renderer, console mode, resolution, upscaler, frame generation, GPU accuracy,
/// anti-aliasing and post-processing.
class GraphicsControls final : public InGamePopover {
    Q_OBJECT

public:
    GraphicsControls(const InGameControls& controls, QWidget* parent);
    void SetSummary(const InGameSummary& summary);
    void SetConsoleMode(bool docked);

signals:
    void DockedChanged(bool docked);

private:
    ValueButton* renderer;
    ValueButton* resolution;
    ValueButton* filter;
    ValueButton* frame_gen;
    ValueButton* accuracy;
    ValueButton* anti_aliasing;
    ValueButton* effects = nullptr;
    SegmentedControl* console_mode;
};

/// Audio: volume, mute and the output device.
class AudioControls final : public InGamePopover {
    Q_OBJECT

public:
    AudioControls(const InGameControls& controls, QWidget* parent);
    void SetVolume(int volume, bool muted);
    void SetOutput(const QString& device);

signals:
    void VolumeChanged(int volume);

private:
    VolumeControl* volume;
    QWidget* mute;
    QLabel* output;
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
