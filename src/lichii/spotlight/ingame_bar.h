// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStatusBar>

class QAbstractButton;
class QLabel;

namespace Spotlight {

class BarChip;
class BarMetrics;
class MenuTooltip;
class ShaderChip;

/// The bottom bar of a windowed game (Paper 14YT-0); it replaces the status bar while a game runs.
class InGameBar final : public QStatusBar {
    Q_OBJECT

public:
    explicit InGameBar(QWidget* parent);
    void SetConfiguration(const QString& renderer, const QString& mode);
    void SetStats(double game_fps, double generated_fps, double frame_ms, bool at_target,
                  const QString& details);
    void SetSpeedStatus(const QString& description);
    void SetMenuShortcut(const QString& keyboard, const QString& controller);
    void SetMenuOpen(bool open);
    void SetQuickControlsOpen(bool open);
    /// Collapsed, the bar is a 6 px strip with a handle (bar-states 15AJ-0); a click expands it.
    void SetCollapsed(bool collapsed);
    QWidget* QuickControlsAnchor() const;
    ShaderChip* Shaders() const {
        return shaders;
    }

signals:
    void MenuRequested();
    void QuickControlsRequested();
    void ExpandRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QWidget* contents;
    QAbstractButton* configuration;
    ShaderChip* shaders;
    BarChip* speed;
    QLabel* notice;
    BarMetrics* metrics;
    BarChip* menu_button;
    MenuTooltip* menu_tooltip;
    bool collapsed = false;
};

} // namespace Spotlight
