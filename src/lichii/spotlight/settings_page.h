// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <functional>
#include <QColor>
#include <QList>
#include <QWidget>
#include "common/settings_enums.h"

class QAbstractButton;
class QComboBox;
class QGroupBox;
class QLabel;
class QLayout;
class QLineEdit;
class QScrollArea;
class QSlider;
class QVBoxLayout;

namespace Settings {
class BasicSetting;
}

namespace ConfigurationShared {
class Widget;
}

namespace Spotlight {

namespace SettingsColor {
inline QColor Green() {
    return QColor(0x7e, 0xe2, 0xa8);
}
inline QColor Gold() {
    return QColor(0xff, 0xd3, 0x7a);
}
inline QColor Blue() {
    return QColor(0x8f, 0xb5, 0xff);
}
inline QColor Lavender() {
    return QColor(0xc9, 0xb3, 0xff);
}
inline QColor BackdropTop() {
    return QColor(0x0f, 0x14, 0x20);
}
inline QColor BackdropBottom() {
    return QColor(0x17, 0x16, 0x19);
}
inline QColor Sidebar() {
    return QColor(0x20, 0x21, 0x25);
}
} // namespace SettingsColor

struct PerformancePreset {
    Settings::ResolutionSetup resolution;
    Settings::ScalingFilter filter;
    Settings::AntiAliasing anti_aliasing;
    bool frame_generation;

    bool operator==(const PerformancePreset&) const = default;
};

inline constexpr std::array PerformancePresets{
    PerformancePreset{Settings::ResolutionSetup::Res3_4X, Settings::ScalingFilter::Bilinear,
                      Settings::AntiAliasing::None, false},
    PerformancePreset{Settings::ResolutionSetup::Res1X, Settings::ScalingFilter::ScaleForce,
                      Settings::AntiAliasing::None, false},
    PerformancePreset{Settings::ResolutionSetup::Res2X, Settings::ScalingFilter::Fsr,
                      Settings::AntiAliasing::Smaa, false},
};

int PerformancePresetIndex(const PerformancePreset& values);
QString PerformancePresetName(int index);

class ToggleSwitch;

class SettingsPage final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent, bool per_game);

    int AddCategory(const QString& title, const QString& subtitle, const QString& icon,
                    const QColor& color, QString page_title = {});
    /// A page without a sidebar entry, reached from a row of its parent category.
    /// A wide page hides the sidebar while it is shown.
    int AddSubPage(int parent, const QString& title, const QString& subtitle, bool wide = false);
    /// Builds the category's rows the first time it is shown or searched.
    void SetBuilder(int category, std::function<void()> build);
    QVBoxLayout* AddSection(int category, const QString& title = {}, bool advanced = false);
    void AddBlock(int category, QWidget* widget, QString search_label, bool advanced = false);
    /// Shows an existing settings page as Spotlight rows, built when first needed. The page stays
    /// hidden and keeps its own load and apply logic; the rows edit its controls.
    void AddPage(int category, QWidget* page, const QString& name, bool advanced = true);
    void SetCategory(int category);
    void ShowAdvancedPage(int category, QWidget* page);
    void SetFooter(QWidget* widget);
    void SetBanner(QWidget* banner);
    void SetGame(const QString& title, const QPixmap& icon);

    static QLabel* Text(const QString& text, int size, int weight = 500, bool secondary = false);
    static QPixmap WholeIcon(const QPixmap& icon, int size, qreal dpr);
    static QWidget* Badge(const QString& text, const QColor& fill, const QColor& ink);
    static QWidget* Card(const QString& icon = {}, int image_size = 0);
    static QLabel* AddRow(QVBoxLayout* group, const QString& title, const QString& description,
                          const QString& icon, const QColor& color, QWidget* control,
                          const QString& tooltip = {});
    static QAbstractButton* Manage(const QString& text = {});
    static ConfigurationShared::Widget* Editor(QWidget* page,
                                               const Settings::BasicSetting& setting);
    static QWidget* BindSetting(ConfigurationShared::Widget* source);
    static QWidget* BindFrameGeneration(QWidget* graphics);
    static QComboBox* BindCombo(QComboBox* source, const QString& placeholder = {},
                                std::function<QString(const QComboBox&, int)> closed_label = {});
    static QString ClosedSettingLabel(const QComboBox* source, u32 setting_id, int index);
    static QWidget* BindToggle(QAbstractButton* source);
    static QWidget* BindToggle(QGroupBox* source);
    static QWidget* BindSpin(QWidget* source);
    static QWidget* BindSlider(QSlider* source);
    static QWidget* BindLineEdit(QLineEdit* source);
    static QAbstractButton* BindButton(QAbstractButton* source, const QString& text = {});
    static void WatchOverride(ConfigurationShared::Widget* source, QWidget* context,
                              std::function<void()> update);
    static void SetComboIndex(QComboBox* source, int index);

signals:
    void BackRequested();

private:
    struct Block {
        QWidget* widget;
        QString search_label;
        bool advanced;
    };
    struct Page {
        QWidget* source;
        QString name;
        bool advanced;
        QWidget* anchor = nullptr;
    };
    struct Category {
        QString title;
        QString page_title;
        QString subtitle;
        QString icon;
        QColor color;
        QAbstractButton* button = nullptr;
        QWidget* panel = nullptr;
        int parent = -1;
        bool wide = false;
        int basic_count = 0;
        QList<Block> blocks;
        QList<Page> pages;
        std::function<void()> build;
        bool basic_built = false;
        bool advanced_built = false;
    };

    void Build(int category, bool with_advanced);
    void BuildPage(int category, Page& page);
    bool RefreshCategory(int category);
    void UpdateAdvanced();
    void UpdateSearch();

    QWidget* navigation;
    QVBoxLayout* sidebar;
    QVBoxLayout* content;
    QVBoxLayout* main_column;
    QScrollArea* scroll;
    QLineEdit* search;
    QWidget* parent_link;
    QLabel* parent_name;
    QLabel* heading;
    QLabel* subtitle;
    QLabel* empty;
    QLabel* game_icon = nullptr;
    QLabel* game_title = nullptr;
    ToggleSwitch* advanced;
    QList<Category> categories;
    int current_category = -1;
    int unfiltered_category = -1;
    bool filtering = false;
    bool building = false;
};

} // namespace Spotlight
