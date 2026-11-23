// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QStatusBar>

class QLabel;

namespace Spotlight {

class BarChip;
class BarMetrics;
class ConfigurationStatus;
class ShaderChip;

/// The bottom bar of a windowed game (Paper 14YT-0); it replaces the status bar while a game runs.
/// It only reports status; the running title bar's menus hold the controls.
class InGameBar final : public QStatusBar {
    Q_OBJECT

public:
    explicit InGameBar(QWidget* parent);
    void SetConfiguration(const QString& renderer, const QString& mode);
    void SetStats(double game_fps, double generated_fps, double frame_ms, bool at_target,
                  const QString& details);
    void SetSpeedStatus(const QString& description);
    /// Collapsed, the bar is a 6 px strip with a handle (bar-states 15AJ-0); a click expands it.
    void SetCollapsed(bool collapsed);
    ShaderChip* Shaders() const {
        return shaders;
    }

signals:
    void ExpandRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    QWidget* contents;
    ConfigurationStatus* configuration;
    ShaderChip* shaders;
    BarChip* speed;
    QLabel* notice;
    BarMetrics* metrics;
    bool collapsed = false;
};

} // namespace Spotlight
