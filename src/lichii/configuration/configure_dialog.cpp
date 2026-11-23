// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <tuple>
#include <utility>
#include <QAbstractSlider>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QVBoxLayout>
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/util/vk.h"
#include "ui_configure.h"
#include "lichii/configuration/configuration_shared.h"
#include "lichii/configuration/configure_applets.h"
#include "lichii/configuration/configure_audio.h"
#include "lichii/configuration/configure_cpu.h"
#include "lichii/configuration/configure_debug_tab.h"
#include "lichii/configuration/configure_dialog.h"
#include "lichii/configuration/configure_filesystem.h"
#include "lichii/configuration/configure_general.h"
#include "lichii/configuration/configure_graphics.h"
#include "lichii/configuration/configure_graphics_advanced.h"
#include "lichii/configuration/configure_graphics_extensions.h"
#include "lichii/configuration/configure_hotkeys.h"
#include "lichii/configuration/configure_input.h"
#include "lichii/configuration/configure_network.h"
#include "lichii/configuration/configure_profile_manager.h"
#include "lichii/configuration/configure_system.h"
#include "lichii/configuration/configure_ui.h"
#include "lichii/configuration/configure_web.h"
#include "lichii/configuration/shared_widget.h"
#include "lichii/hotkeys.h"
#include "lichii/spotlight/settings_get_started.h"
#include "lichii/spotlight/settings_page.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace {
using Spotlight::PresetCard;

class ColourSwatch final : public QAbstractButton {
public:
    ColourSwatch(QPushButton* source_, const QString& name) : source{source_} {
        setFixedSize(28, 28);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(name);
        setToolTip(name);
        connect(this, &QAbstractButton::clicked, source, &QAbstractButton::click);
        source->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::StyleChange) {
            update();
        }
        return QAbstractButton::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent*) override {
        static const QRegularExpression pattern(
            QStringLiteral("background-color:\\s*(#[0-9a-fA-F]{6})"));
        const auto match = pattern.match(source->styleSheet());
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool active = hasFocus() || underMouse();
        painter.setPen(
            QPen(active ? Spotlight::Color::Accent() : QColor(255, 255, 255, 70), 1.5));
        painter.setBrush(match.hasMatch() ? QColor(match.captured(1)) : QColor(Qt::transparent));
        painter.drawEllipse(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5));
    }

    void enterEvent(QEnterEvent* event) override {
        update();
        QAbstractButton::enterEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        update();
        QAbstractButton::leaveEvent(event);
    }

private:
    QPushButton* source;
};
using Spotlight::SettingsPage;
namespace Color = Spotlight::Color;
namespace Tints = Spotlight::SettingsColor;

int SuggestedTier(const VkDeviceInfo::Record* device) {
    if (device == nullptr) {
        return 0;
    }
    const QString name = QString::fromStdString(device->name);
    static const QRegularExpression high(
        QStringLiteral(R"(RTX\s+[345]\d{3}\b|RX\s+[67][789]\d{2}\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression mid(
        QStringLiteral(
            R"(RTX\s+\d{4}\b|GTX\s+\d{4}\b|RX\s+\d{3,4}\b|Arc\s+[AB]\d{3}\b|Apple M[1-9])"),
        QRegularExpression::CaseInsensitiveOption);
    if (device->fsr4_capable || high.match(name).hasMatch()) {
        return 2;
    }
    return mid.match(name).hasMatch() ? 1 : 0;
}

QString MappingStyle() {
    return QStringLiteral(
        "QTabWidget#spotlightMapping::pane { border: none; background: transparent; }"
        "QTabWidget#spotlightMapping > QStackedWidget > QWidget { background: transparent; }"
        "QTabWidget#spotlightMapping QGroupBox { background: rgba(255,255,255,13);"
        " border: 1px solid rgba(255,255,255,20); border-radius: 14px; margin-top: 22px;"
        " padding: 8px 4px 4px 4px; font-size: 12px; font-weight: 600; }"
        "QTabWidget#spotlightMapping QGroupBox::title { subcontrol-origin: margin; left: 8px;"
        " top: 2px; color: rgba(255,255,255,158); background: transparent; }"
        "QTabWidget#spotlightMapping QGroupBox QGroupBox { background: transparent; border: none;"
        " margin-top: 16px; padding: 2px; }"
        "QTabWidget#spotlightMapping QPushButton { background: rgba(255,255,255,20);"
        " color: #ffffff;"
        " border: 1px solid transparent; border-radius: 10px; min-height: 28px; min-width: 52px;"
        " padding: 0 6px;"
        " font-size: 12px; font-weight: 600; }"
        "QTabWidget#spotlightMapping QPushButton:hover { background: rgba(255,255,255,46); }"
        "QTabWidget#spotlightMapping QPushButton:focus { border-color: #ff6b95; }"
        "QTabWidget#spotlightMapping QComboBox { background: rgba(255,255,255,20); color: #ffffff;"
        " border: 1px solid transparent; border-radius: 12px; min-height: 30px;"
        " padding: 0 12px; font-size: 13px; font-weight: 600; }"
        "QTabWidget#spotlightMapping QLabel { color: rgba(255,255,255,158);"
        " background: transparent; }");
}

class BindingButton final : public QAbstractButton {
public:
    explicit BindingButton(QString icon_) : icon{std::move(icon_)} {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setProperty("spotlightTransparent", true);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(30, 5, 6, 5);
        cap = new Spotlight::KeyCap({});
        cap->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(cap);
    }

    void SetBinding(const QString& text, bool set) {
        cap->SetText(text);
        cap->setEnabled(set);
        updateGeometry();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(hasFocus() ? QPen(Color::Accent(), 1.5) : Qt::NoPen);
        painter.setBrush(underMouse() ? Color::Fill() : Color::FillFaint());
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75), 12, 12);
        painter.drawPixmap(9, (height() - 14) / 2,
                           Spotlight::IconPixmap(icon, 14, Color::TextSecondary(),
                                                 devicePixelRatioF()));
    }

private:
    QString icon;
    Spotlight::KeyCap* cap;
};

bool IsBuiltInFolder(const UISettings::GameDir& folder) {
    return folder.path == "SDMC" || folder.path == "UserNAND" || folder.path == "SysNAND";
}

} // namespace

ConfigureDialog::ConfigureDialog(QWidget* parent, HotkeyRegistry& registry_,
                                 InputCommon::InputSubsystem* input_subsystem_,
                                 std::vector<VkDeviceInfo::Record>& vk_device_records,
                                 Core::System& system_, bool enable_web_config)
    : QDialog(parent), ui{std::make_unique<Ui::ConfigureDialog>()}, registry(registry_),
      system{system_}, devices{vk_device_records}, input_subsystem{input_subsystem_},
      builder{std::make_unique<ConfigurationShared::Builder>(this, !system_.IsPoweredOn())},
      applets_tab{std::make_unique<ConfigureApplets>(system_, nullptr, *builder, this)},
      audio_tab{std::make_unique<ConfigureAudio>(system_, nullptr, *builder, this)},
      cpu_tab{std::make_unique<ConfigureCpu>(system_, nullptr, *builder, this)},
      debug_tab_tab{std::make_unique<ConfigureDebugTab>(system_, this)},
      filesystem_tab{std::make_unique<ConfigureFilesystem>(this)},
      general_tab{std::make_unique<ConfigureGeneral>(system_, nullptr, *builder, this)},
      graphics_advanced_tab{
          std::make_unique<ConfigureGraphicsAdvanced>(system_, nullptr, *builder, this)},
      graphics_extensions_tab{
          std::make_unique<ConfigureGraphicsExtensions>(system_, nullptr, *builder, this)},
      ui_tab{std::make_unique<ConfigureUi>(system_, this)},
      graphics_tab{std::make_unique<ConfigureGraphics>(
          system_, vk_device_records, [&]() { graphics_advanced_tab->ExposeComputeOption(); },
          [this](Settings::AspectRatio ratio, Settings::ResolutionSetup setup) {
              ui_tab->UpdateScreenshotInfo(ratio, setup);
          },
          nullptr, *builder, this)},
      hotkeys_tab{std::make_unique<ConfigureHotkeys>(system_.HIDCore(), this)},
      input_tab{std::make_unique<ConfigureInput>(system_, this)},
      network_tab{std::make_unique<ConfigureNetwork>(system_, this)},
      profile_tab{std::make_unique<ConfigureProfileManager>(system_, this)},
      system_tab{std::make_unique<ConfigureSystem>(system_, nullptr, *builder, this)},
      web_tab{std::make_unique<ConfigureWeb>(this)} {
    Settings::SetConfiguringGlobal(true);

    ui->setupUi(this);
    Spotlight::FadeInOnShow(this);

    web_tab->SetWebServiceConfigEnabled(enable_web_config);
    hotkeys_tab->Populate(registry);
    game_dirs = UISettings::values.game_dirs;
    general_tab->SetResetCallback([this] { done(QDialog::Rejected); });
    connect(ui_tab.get(), &ConfigureUi::LanguageChanged, this, &ConfigureDialog::OnLanguageChanged);
    connect(general_tab.get(), &ConfigureGeneral::ExternalContentDirsChanged, this,
            &ConfigureDialog::ExternalContentDirsChanged);

    ui->footer->setVisible(system.IsPoweredOn());
    BuildSettings();
    ConfigurationShared::FitToScreen(this, QSize(1280, 800));
}

ConfigureDialog::~ConfigureDialog() = default;

void ConfigureDialog::ApplyConfiguration() {
    general_tab->ApplyConfiguration();
    ui_tab->ApplyConfiguration();
    system_tab->ApplyConfiguration();
    profile_tab->ApplyConfiguration();
    filesystem_tab->ApplyConfiguration();
    if (input_initialized) {
        input_tab->ApplyConfiguration();
        auto& players = Settings::values.players.GetValue();
        for (int i = 0; i < vibration_strength.size(); ++i) {
            players[i].vibration_strength = vibration_strength[i];
        }
    }
    hotkeys_tab->ApplyConfiguration(registry);
    cpu_tab->ApplyConfiguration();
    graphics_tab->ApplyConfiguration();
    graphics_advanced_tab->ApplyConfiguration();
    graphics_extensions_tab->ApplyConfiguration();
    audio_tab->ApplyConfiguration();
    debug_tab_tab->ApplyConfiguration();
    web_tab->ApplyConfiguration();
    network_tab->ApplyConfiguration();
    applets_tab->ApplyConfiguration();
    if (check_updates != nullptr) {
        UISettings::values.check_for_updates.SetValue(check_updates->isChecked());
    }
    if (start_fullscreen != nullptr && start_fullscreen->isEnabled() &&
        UISettings::values.fullscreen.GetValue() != start_fullscreen->isChecked()) {
        UISettings::values.fullscreen.SetValue(start_fullscreen->isChecked());
        emit FullscreenOnStartChanged(start_fullscreen->isChecked());
    }
    if (game_dirs_changed) {
        UISettings::values.game_dirs = game_dirs;
        UISettings::values.is_game_list_reload_pending = true;
        game_dirs_changed = false;
    }
    system.ApplySettings();
    Settings::LogSettings();
}

void ConfigureDialog::RestoreInputConfiguration() {
    if (input_initialized) {
        input_tab->RestoreConfiguration();
    }
}

void ConfigureDialog::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
    }
    QDialog::changeEvent(event);
}

void ConfigureDialog::reject() {
    accept();
}

void ConfigureDialog::OnLanguageChanged(const QString& locale) {
    emit LanguageChanged(locale);
    UISettings::values.is_game_list_reload_pending = true;
    ApplyConfiguration();
    reopen_requested = true;
    accept();
}

void ConfigureDialog::BuildSettings() {
    settings_page = new SettingsPage(this, false);
    for (const char* name :
         {"external_content_list", "add_external_dir_button", "remove_external_dir_button"}) {
        general_tab->findChild<QWidget*>(QLatin1String(name))->setProperty("spotlightBound", true);
    }
    ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
    ui->verticalLayout->setSpacing(0);
    ui->verticalLayout->addWidget(settings_page);
    settings_page->SetFooter(ui->footer);
    connect(settings_page, &SettingsPage::BackRequested, this, &QDialog::accept);

    const int general =
        settings_page->AddCategory(tr("General"), tr("Preset, look, folders and updates"),
                                   QStringLiteral("globe"), Color::Accent());
    const int library = settings_page->AddCategory(
        tr("Library"), tr("Choose where Lichii looks for games and what the library shows"),
        QStringLiteral("folder"), Tints::Green());
    const int graphics = settings_page->AddCategory(
        tr("Graphics"), tr("Preset first; everything else follows it unless you change it"),
        QStringLiteral("monitor"), Tints::Gold());
    const int audio = settings_page->AddCategory(
        tr("Audio"), tr("Choose where game audio plays and how loud it is"),
        QStringLiteral("speaker-high"), Tints::Blue());
    const int controls = settings_page->AddCategory(
        tr("Controls"), tr("One controller per player; hotkeys work with keyboard and pad"),
        QStringLiteral("game-controller"), Tints::Lavender());
    const int system_page = settings_page->AddCategory(
        tr("System"), tr("Choose the emulated console's language, region and storage"),
        QStringLiteral("cpu"), Color::Accent());
    const int multiplayer = settings_page->AddCategory(
        tr("Multiplayer"), tr("Configure local networking and online services"),
        QStringLiteral("users"), Tints::Green());
    const int shortcuts =
        settings_page->AddCategory(tr("Shortcuts"), tr("Edit keyboard and controller shortcuts"),
                                   QStringLiteral("keyboard"), Tints::Gold());

    settings_page->SetBuilder(general, [this, general, library, controls] {
        BuildGeneral(general, library, controls);
    });
    settings_page->SetBuilder(library, [this, library] { BuildLibrary(library); });
    settings_page->SetBuilder(graphics, [this, graphics] { BuildGraphics(graphics); });
    settings_page->SetBuilder(audio, [this, audio] {
        auto* output = settings_page->AddSection(audio, tr("Output"));
        AddSettingRow(output, audio_tab.get(), Settings::values.volume, tr("Volume"),
                      tr("Game audio level"), QStringLiteral("speaker-high"), Tints::Blue());
        AddSettingRow(output, audio_tab.get(), Settings::values.sink_id, tr("Audio engine"),
                      tr("Backend for game audio"), QStringLiteral("sliders-horizontal"),
                      Tints::Lavender());
        AddSettingRow(output, audio_tab.get(), Settings::values.audio_output_device_id,
                      tr("Output device"), tr("Speaker or headphone destination"),
                      QStringLiteral("speaker-high"), Tints::Gold());
    });
    settings_page->SetBuilder(controls,
                              [this, controls, shortcuts] { BuildControls(controls, shortcuts); });
    settings_page->SetBuilder(system_page, [this, system_page] {
        auto* console = settings_page->AddSection(system_page, tr("Console"));
        AddSettingRow(console, system_tab.get(), Settings::values.language_index,
                      tr("System language"), tr("Language reported to games"),
                      QStringLiteral("translate"), Tints::Blue());
        AddSettingRow(console, system_tab.get(), Settings::values.region_index, tr("Region"),
                      tr("Region reported to games"), QStringLiteral("globe"), Color::Accent());
        const int profiles = settings_page->AddSubPage(
            system_page, tr("Profiles"), tr("Local console users; games start as the current one"));
        settings_page->SetBuilder(profiles, [this, profiles] { BuildProfiles(profiles); });
        auto* profile = SettingsPage::Manage(tr("Manage"));
        connect(profile, &QAbstractButton::clicked, settings_page,
                [this, profiles] { settings_page->SetCategory(profiles); });
        SettingsPage::AddRow(console, tr("Profiles"), tr("Manage local console profiles"),
                             QStringLiteral("user"), Tints::Lavender(), profile);
    });
    settings_page->SetBuilder(multiplayer, [this, multiplayer] {
        auto* networking = settings_page->AddSection(multiplayer, tr("Local network"));
        SettingsPage::AddRow(networking, tr("Network interface"),
                             tr("Connection to use for multiplayer"), QStringLiteral("wifi-high"),
                             Tints::Blue(),
                             SettingsPage::BindCombo(network_tab->findChild<QComboBox*>(
                                 QStringLiteral("network_interface"))));
        SettingsPage::AddRow(networking, tr("LAN play"), tr("Enable local network play"),
                             QStringLiteral("users"), Tints::Green(),
                             SettingsPage::BindToggle(
                                 network_tab->findChild<QCheckBox*>(QStringLiteral("lan_play"))));
    });

    for (const auto& [category, widget, name] :
         std::initializer_list<std::tuple<int, QWidget*, QString>>{
             {general, general_tab.get(), tr("General")},
             {general, debug_tab_tab.get(), tr("Debug")},
             {library, ui_tab.get(), tr("Interface and game list")},
             {graphics, graphics_tab.get(), tr("Graphics")},
             {graphics, graphics_advanced_tab.get(), tr("Advanced graphics")},
             {graphics, graphics_extensions_tab.get(), tr("Extras")},
             {audio, audio_tab.get(), tr("Audio")},
             {system_page, system_tab.get(), tr("System")},
             {system_page, cpu_tab.get(), tr("CPU")},
             {system_page, filesystem_tab.get(), tr("Filesystem")},
             {system_page, applets_tab.get(), tr("Applets")},
             {multiplayer, network_tab.get(), tr("Network")},
             {multiplayer, web_tab.get(), tr("Web")},
         }) {
        settings_page->AddPage(category, widget, name);
    }
    settings_page->SetBuilder(shortcuts, [this, shortcuts] { BuildShortcuts(shortcuts); });
    hotkeys_tab->hide();
    profile_tab->hide();
    input_tab->hide();
    settings_page->SetCategory(general);
}

void ConfigureDialog::BuildProfiles(int profiles) {
    profile_rows = settings_page->AddSection(profiles, tr("Users"));
    auto* actions = new QWidget;
    auto* actions_layout = new QHBoxLayout(actions);
    actions_layout->setContentsMargins(0, 4, 0, 0);
    actions_layout->setSpacing(14);
    add_profile = new Spotlight::PillButton(tr("Add a profile"), QStringLiteral("plus"),
                                            Spotlight::PillButton::Kind::Accent, 38);
    connect(add_profile, &QAbstractButton::clicked, this, [this] { profile_tab->AddUser(); });
    actions_layout->addWidget(add_profile);
    if (system.IsPoweredOn()) {
        actions_layout->addWidget(SettingsPage::Text(
            tr("Changes reach the running game after it restarts."), 13, 500, true));
    }
    actions_layout->addStretch();
    settings_page->AddBlock(profiles, actions, tr("Add a profile"));
    auto* model = profile_tab->Model();
    connect(model, &QAbstractItemModel::rowsInserted, this, &ConfigureDialog::RebuildProfiles);
    connect(model, &QAbstractItemModel::rowsRemoved, this, &ConfigureDialog::RebuildProfiles);
    connect(model, &QAbstractItemModel::dataChanged, this, &ConfigureDialog::RebuildProfiles);
    connect(profile_tab.get(), &ConfigureProfileManager::CurrentUserChanged, this,
            &ConfigureDialog::RebuildProfiles);
    RebuildProfiles();
}

void ConfigureDialog::RebuildProfiles() {
    while (auto* item = profile_rows->takeAt(0)) {
        item->widget()->hide();
        item->widget()->deleteLater();
        delete item;
    }
    auto* model = profile_tab->Model();
    const int current = Settings::values.current_user.GetValue();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QStandardItem* user = model->item(row);
        const QStringList lines = user->text().split(QChar::fromLatin1('\n'));
        auto* control = new QWidget;
        auto* layout = new QHBoxLayout(control);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        if (row == current) {
            layout->addWidget(
                SettingsPage::Badge(tr("Current"), Color::Accent(), Color::OnAccent()), 0,
                Qt::AlignVCenter);
        } else {
            auto* use = new Spotlight::PillButton(tr("Make current"));
            connect(use, &QAbstractButton::clicked, this,
                    [this, row] { profile_tab->SelectUserAt(row); });
            layout->addWidget(use);
        }
        auto* more = new Spotlight::CircleButton(QStringLiteral("dots-three"), 32);
        more->setAccessibleName(tr("More for %1").arg(lines.value(0)));
        auto* menu = new QMenu(more);
        menu->addAction(tr("Edit name and picture"), this,
                        [this, row] { profile_tab->EditUserAt(row); });
        menu->addAction(tr("Delete"), this, [this, row] { profile_tab->ConfirmDeleteUserAt(row); });
        connect(more, &QAbstractButton::clicked, menu,
                [menu, more] { menu->popup(more->mapToGlobal(QPoint(0, more->height()))); });
        layout->addWidget(more);
        SettingsPage::AddRow(profile_rows, lines.value(0), lines.value(1), QStringLiteral("user"),
                             Tints::Lavender(), control, {}, user->icon().pixmap(64, 64));
    }
    add_profile->setEnabled(profile_tab->CanAddUser());
}

void ConfigureDialog::BuildShortcuts(int shortcuts) {
    auto* actions = new QWidget;
    actions->setProperty("spotlightTransparent", true);
    auto* actions_layout = new QHBoxLayout(actions);
    actions_layout->setContentsMargins(6, 0, 0, 0);
    actions_layout->setSpacing(10);
    actions_layout->addWidget(SettingsPage::Text(
        tr("Click a binding to change it; a game controller binding is pressed on the pad"), 13,
        500, true));
    actions_layout->addStretch();
    auto* clear_all = new Spotlight::PillButton(tr("Clear all"), QStringLiteral("x"));
    connect(clear_all, &QAbstractButton::clicked, this, [this] { hotkeys_tab->ClearAll(); });
    actions_layout->addWidget(clear_all);
    auto* restore = new Spotlight::PillButton(tr("Restore defaults"),
                                              QStringLiteral("arrow-counter-clockwise"));
    connect(restore, &QAbstractButton::clicked, this, [this] { hotkeys_tab->RestoreDefaults(); });
    actions_layout->addWidget(restore);
    settings_page->AddBlock(shortcuts, actions, tr("Clear all\nRestore defaults"));

    auto* model = hotkeys_tab->Model();
    for (int group_row = 0; group_row < model->rowCount(); ++group_row) {
        const QStandardItem* group = model->item(group_row);
        auto* rows = settings_page->AddSection(shortcuts, group->text());
        for (int row = 0; row < group->rowCount(); ++row) {
            const QPersistentModelIndex action = group->child(row)->index();
            auto* control = new QWidget;
            auto* layout = new QHBoxLayout(control);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(8);
            auto* keyboard = new BindingButton(QStringLiteral("keyboard"));
            auto* pad = new BindingButton(QStringLiteral("game-controller"));
            auto* reset =
                new Spotlight::CircleButton(QStringLiteral("arrow-counter-clockwise"), 32);
            reset->setToolTip(tr("Restore default"));
            reset->setAccessibleName(reset->toolTip());
            reset->setFocusPolicy(Qt::StrongFocus);
            keyboard->setMinimumWidth(128);
            pad->setMinimumWidth(184);
            for (auto* button : {keyboard, pad}) {
                layout->addWidget(button);
            }
            layout->addWidget(reset);
            const auto refresh = [this, action, keyboard, pad, reset] {
                const QString key = action.sibling(action.row(), 1).data().toString();
                const QString button = action.sibling(action.row(), 2).data().toString();
                keyboard->SetBinding(key.isEmpty() ? tr("Not set") : key, !key.isEmpty());
                keyboard->setToolTip(tr("Keyboard: %1").arg(key.isEmpty() ? tr("not set") : key));
                keyboard->setAccessibleName(keyboard->toolTip());
                pad->SetBinding(button.isEmpty() ? tr("Not set") : button, !button.isEmpty());
                pad->setToolTip(
                    tr("Controller: %1").arg(button.isEmpty() ? tr("not set") : button));
                pad->setAccessibleName(pad->toolTip());
                reset->setVisible(!hotkeys_tab->IsDefault(action));
            };
            refresh();
            connect(model, &QAbstractItemModel::dataChanged, control, refresh);
            connect(keyboard, &QAbstractButton::clicked, this,
                    [this, action] { hotkeys_tab->Configure(action.sibling(action.row(), 1)); });
            connect(pad, &QAbstractButton::clicked, this,
                    [this, action] { hotkeys_tab->Configure(action.sibling(action.row(), 2)); });
            connect(reset, &QAbstractButton::clicked, this,
                    [this, action] { hotkeys_tab->RestoreAction(action); });
            SettingsPage::AddRow(rows, group->child(row)->text(), {}, QStringLiteral("keyboard"),
                                 Tints::Gold(), control);
        }
    }
}

void ConfigureDialog::AddSettingRow(QVBoxLayout* group, QWidget* page,
                                    const Settings::BasicSetting& setting, const QString& title,
                                    const QString& detail, const QString& icon,
                                    const QColor& color) {
    if (auto* source = SettingsPage::Editor(page, setting)) {
        if (auto* control = SettingsPage::BindSetting(source)) {
            SettingsPage::AddRow(group, title, detail, icon, color, control);
        }
    }
}

ConfigureDialog::PresetSources ConfigureDialog::FindPresetSources() const {
    const auto& values = Settings::values;
    auto* gfx = graphics_tab.get();
    auto* resolution = SettingsPage::Editor(gfx, values.resolution_setup);
    auto* filter = SettingsPage::Editor(gfx, values.scaling_filter);
    auto* aa = SettingsPage::Editor(gfx, values.anti_aliasing);
    auto* frame_gen = SettingsPage::Editor(gfx, values.frame_gen);
    auto* device = SettingsPage::Editor(gfx, values.vulkan_device);
    if (!resolution || !filter || !aa || !frame_gen) {
        return {};
    }
    return {resolution->combobox, filter->combobox, aa->combobox, frame_gen->checkbox,
            device ? device->combobox : nullptr};
}

bool ConfigureDialog::PresetSources::CanApply() const {
    return resolution->isEnabled() && filter->isEnabled() && aa->isEnabled() &&
           frame_generation->isEnabled();
}

const VkDeviceInfo::Record* ConfigureDialog::SelectedGpu(const PresetSources& sources) const {
    const int index = sources.device ? sources.device->currentIndex() : -1;
    return index >= 0 && std::cmp_less(index, devices.size()) ? &devices[index] : nullptr;
}

int ConfigureDialog::CurrentPreset(const PresetSources& sources) const {
    const int selected = Spotlight::PerformancePresetIndex({
        static_cast<Settings::ResolutionSetup>(sources.resolution->currentData().toUInt()),
        static_cast<Settings::ScalingFilter>(sources.filter->currentData().toUInt()),
        static_cast<Settings::AntiAliasing>(sources.aa->currentData().toUInt()),
        sources.frame_generation->isChecked(),
    });
    if (selected == -1) {
        return -1;
    }
    return selected == SuggestedTier(SelectedGpu(sources)) ? 0 : selected + 1;
}

void ConfigureDialog::ApplyPreset(const PresetSources& sources, int selection) {
    const auto& preset =
        Spotlight::PerformancePresets[selection == 0 ? SuggestedTier(SelectedGpu(sources))
                                                     : selection - 1];
    SettingsPage::SetComboIndex(sources.resolution,
                                sources.resolution->findData(static_cast<u32>(preset.resolution)));
    SettingsPage::SetComboIndex(sources.filter,
                                sources.filter->findData(static_cast<u32>(preset.filter)));
    SettingsPage::SetComboIndex(sources.aa,
                                sources.aa->findData(static_cast<u32>(preset.anti_aliasing)));
    if (sources.frame_generation->isChecked() != preset.frame_generation) {
        sources.frame_generation->click();
    }
}

void ConfigureDialog::WatchPreset(const PresetSources& sources, QObject* context,
                                  const std::function<void()>& sync) {
    for (auto* combo : {sources.resolution, sources.filter, sources.aa}) {
        connect(combo, &QComboBox::currentIndexChanged, context, sync);
    }
    connect(sources.frame_generation, &QCheckBox::toggled, context, sync);
    if (sources.device != nullptr) {
        connect(sources.device, &QComboBox::currentIndexChanged, context, sync);
    }
    sync();
}

void ConfigureDialog::BuildGeneral(int general, int library, int controls) {
    const auto sources = FindPresetSources();
    QList<PresetCard*> presets;
    if (sources.resolution != nullptr) {
        auto* preset_group = new QWidget;
        auto* presets_layout = new QVBoxLayout(preset_group);
        presets_layout->setContentsMargins(0, 0, 0, 0);
        presets_layout->setSpacing(2);
        auto* presets_label = SettingsPage::Text(
            tr("Preset · changes inherited settings only; game overrides stay"), 13, 600, true);
        presets_label->setContentsMargins(6, 4, 6, 6);
        presets_layout->addWidget(presets_label);
        auto* cards_layout = new QHBoxLayout;
        cards_layout->setContentsMargins(0, 0, 0, 0);
        cards_layout->setSpacing(10);
        const std::array cards{
            new PresetCard(tr("Auto"), tr("Picks for your GPU")),
            new PresetCard(Spotlight::PerformancePresetName(0), tr("Older or integrated GPUs")),
            new PresetCard(Spotlight::PerformancePresetName(1), tr("Most gaming laptops")),
            new PresetCard(Spotlight::PerformancePresetName(2), tr("Desktop GPUs")),
        };
        for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
            cards_layout->addWidget(cards[i], 1);
            connect(cards[i], &QAbstractButton::clicked, this, [this, sources, cards, i] {
                ApplyPreset(sources, i);
                cards[i]->setChecked(CurrentPreset(sources) == i);
            });
        }
        presets_layout->addLayout(cards_layout);
        presets = {cards.cbegin(), cards.cend()};
        settings_page->AddBlock(general, preset_group, tr("Performance preset"));
        WatchPreset(sources, preset_group, [this, sources, cards] {
            const auto* gpu = SelectedGpu(sources);
            cards[0]->SetSuggestion(
                gpu ? QString::fromStdString(gpu->name) : tr("No GPU identified"),
                tr(" → %1").arg(Spotlight::PerformancePresetName(SuggestedTier(gpu))));
            const int selected = CurrentPreset(sources);
            const bool enabled = sources.CanApply();
            for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
                cards[i]->setChecked(i == selected);
                cards[i]->setEnabled(enabled);
            }
        });
    }

    auto* view = ui_tab.get();
    auto* options = settings_page->AddSection(general);
    auto* language = view->findChild<QComboBox*>(QStringLiteral("language_combobox"));
    const auto locale_name = [](const QString& name) {
        return name.startsWith(QChar::fromLatin1('<')) ? tr("System language") : name;
    };
    auto* locale_description = SettingsPage::AddRow(
        options, tr("Language"), locale_name(language->currentText()), QStringLiteral("globe"),
        Color::Accent(),
        SettingsPage::BindCombo(language, {}, [](const QComboBox& combo, int index) {
            const auto locale = combo.itemData(index).toString();
            if (locale.isEmpty()) {
                return tr("System");
            }
            const QLocale parsed(locale);
            return parsed.language() == QLocale::C ? combo.itemText(index)
                                                   : QLocale::languageToString(parsed.language());
        }));
    connect(language, &QComboBox::currentTextChanged, locale_description,
            [locale_description, locale_name](const QString& name) {
                locale_description->setText(locale_name(name));
            });

    auto* theme = view->findChild<QComboBox*>(QStringLiteral("theme_combobox"));
    const int dark_index = theme->findData(QStringLiteral("lichii"));
    const int light_index = theme->findData(QStringLiteral("default"));
    const QStringList looks{tr("Dark"), tr("Light")};
    auto* look =
        new Spotlight::SegmentedControl(looks, Spotlight::SegmentedControl::Kind::Accent, 38);
    const auto sync_theme = [look, theme, dark_index, light_index, looks] {
        const QSignalBlocker blocker(look);
        const int selected = theme->currentIndex() == dark_index    ? 0
                             : theme->currentIndex() == light_index ? 1
                                                                    : 2;
        QStringList items = looks;
        if (selected == 2) {
            items.append(tr("Other"));
        }
        look->SetOptions(items);
        look->SetCurrentIndex(selected);
        look->SetOptionEnabled(2, false);
    };
    sync_theme();
    connect(theme, &QComboBox::currentIndexChanged, look, sync_theme);
    connect(look, &Spotlight::SegmentedControl::CurrentChanged, theme,
            [theme, dark_index, light_index](int index) {
                SettingsPage::SetComboIndex(theme, index == 0 ? dark_index : light_index);
            });
    SettingsPage::AddRow(options, tr("Look"), tr("Choose the window's color theme"),
                         QStringLiteral("palette"), Tints::Lavender(), look);
    auto* manage_folders = SettingsPage::Manage(tr("Manage"));
    connect(manage_folders, &QAbstractButton::clicked, settings_page,
            [this, library] { settings_page->SetCategory(library); });
    folder_summary = SettingsPage::AddRow(options, tr("Game folders"), GameFolderSummary(),
                                          QStringLiteral("folder"), Tints::Green(), manage_folders);
#ifdef ENABLE_UPDATE_CHECKER
    check_updates = new Spotlight::ToggleSwitch;
    check_updates->setChecked(UISettings::values.check_for_updates.GetValue());
    SettingsPage::AddRow(options, tr("Updates"), tr("Check for new Lichii builds at start"),
                         QStringLiteral("cloud-arrow-down"), Tints::Blue(), check_updates);
#endif
    auto* get_started = SettingsPage::Manage();
    connect(get_started, &QAbstractButton::clicked, this,
            [this, controls, presets] { OpenGetStarted(controls, presets); });
    SettingsPage::AddRow(options, tr("Get Started"),
                         tr("Run the setup again: keys, firmware, folders, controller, preset"),
                         QStringLiteral("sparkle"), Tints::Lavender(), get_started);
}

void ConfigureDialog::BuildLibrary(int library) {
    folders_layout = settings_page->AddSection(library, tr("Game folders"));
    RebuildGameFolders();
    auto* external_list =
        general_tab->findChild<QListWidget*>(QStringLiteral("external_content_list"));
    auto* add_external =
        general_tab->findChild<QPushButton*>(QStringLiteral("add_external_dir_button"));
    auto* remove_external =
        general_tab->findChild<QPushButton*>(QStringLiteral("remove_external_dir_button"));
    auto* external_rows = settings_page->AddSection(library, tr("Update and DLC folders"));
    const auto rebuild_external = [external_rows, external_list, add_external, remove_external] {
        while (auto* item = external_rows->takeAt(0)) {
            item->widget()->hide();
            item->widget()->deleteLater();
            delete item;
        }
        for (int i = 0; i < external_list->count(); ++i) {
            const QString path = external_list->item(i)->text();
            auto* remove = new Spotlight::CircleButton(QStringLiteral("trash"), 36);
            remove->setAccessibleName(tr("Remove"));
            remove->setToolTip(remove->accessibleName());
            connect(remove, &QAbstractButton::clicked, external_list,
                    [external_list, remove_external, path] {
                        const auto found = external_list->findItems(path, Qt::MatchExactly);
                        if (!found.isEmpty()) {
                            external_list->setCurrentItem(found.first());
                            remove_external->click();
                        }
                    });
            SettingsPage::AddRow(external_rows, QDir(path).dirName(), path,
                                 QStringLiteral("folder"), Tints::Lavender(), remove);
        }
        auto* add = new Spotlight::PillButton(tr("Add folder"), QStringLiteral("plus"));
        connect(add, &QAbstractButton::clicked, add_external, &QAbstractButton::click);
        SettingsPage::AddRow(external_rows, tr("Add an update and DLC folder"),
                             tr("Lichii reads updates and DLC from it without installing them"),
                             QStringLiteral("folder-open"), Tints::Lavender(), add);
    };
    rebuild_external();
    auto* external_model = external_list->model();
    for (const auto signal :
         {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
        connect(external_model, signal, external_rows->parentWidget(), rebuild_external);
    }
    connect(external_model, &QAbstractItemModel::modelReset, external_rows->parentWidget(),
            rebuild_external);
    auto* view = ui_tab.get();
    auto* display = settings_page->AddSection(library, tr("Display"));
    for (const auto& [name, label] : std::array{
             std::pair{"folder_icon_size_combobox", tr("Folder icon size")},
             std::pair{"row_1_text_combobox", tr("First title line")},
             std::pair{"row_2_text_combobox", tr("Second title line")},
         }) {
        SettingsPage::AddRow(
            display, label, {}, QStringLiteral("list"), Tints::Green(),
            SettingsPage::BindCombo(view->findChild<QComboBox*>(QString::fromLatin1(name))));
    }
    for (const auto& [name, label] : std::array{
             std::pair{"show_add_ons", tr("Show add-ons")},
             std::pair{"show_size", tr("Show size")},
             std::pair{"show_types", tr("Show file formats")},
             std::pair{"show_play_time", tr("Show play time")},
         }) {
        SettingsPage::AddRow(
            display, label, {}, QStringLiteral("eye"), Tints::Blue(),
            SettingsPage::BindToggle(view->findChild<QCheckBox*>(QString::fromLatin1(name))));
    }
}

void ConfigureDialog::BuildGraphics(int graphics) {
    auto& values = Settings::values;
    auto* gfx = graphics_tab.get();
    auto* quick = settings_page->AddSection(graphics, tr("Quick"));
    auto* picture = settings_page->AddSection(graphics, tr("Picture"));
    const auto sources = FindPresetSources();
    if (sources.resolution != nullptr) {
        auto* presets_control = new Spotlight::SegmentedControl(
            {tr("Auto"), tr("Low"), tr("Mid"), tr("High")},
            Spotlight::SegmentedControl::Kind::Accent, 38);
        auto* preset_summary =
            SettingsPage::AddRow(quick, tr("Preset"), {}, QStringLiteral("sliders-horizontal"),
                                 Color::Accent(), presets_control);
        connect(presets_control, &Spotlight::SegmentedControl::CurrentChanged, this,
                [this, sources](int selection) {
                    if (selection < 4) {
                        ApplyPreset(sources, selection);
                    }
                });
        WatchPreset(sources, presets_control, [this, sources, presets_control, preset_summary] {
            const int selected = CurrentPreset(sources);
            const QSignalBlocker blocker(presets_control);
            QStringList choices{tr("Auto"), tr("Low"), tr("Mid"), tr("High")};
            if (selected == -1) {
                choices.append(tr("Custom"));
            }
            presets_control->SetOptions(choices);
            presets_control->SetOptionEnabled(4, false);
            presets_control->SetCurrentIndex(selected == -1 ? 4 : selected);
            presets_control->setEnabled(sources.CanApply());
            const int tier = selected == 0 ? SuggestedTier(SelectedGpu(sources)) : selected - 1;
            const QString resolution = SettingsPage::ClosedSettingLabel(
                sources.resolution, Settings::values.resolution_setup.Id(),
                sources.resolution->currentIndex());
            const QString filter = SettingsPage::ClosedSettingLabel(
                sources.filter, Settings::values.scaling_filter.Id(),
                sources.filter->currentIndex());
            preset_summary->setText(
                selected == -1 ? tr("Custom graphics settings")
                               : tr("%1: %2 resolution, %3, %4. Game overrides stay.")
                                     .arg(Spotlight::PerformancePresetName(tier), resolution,
                                          filter, sources.aa->currentText()));
            preset_summary->show();
        });
    }
    AddSettingRow(quick, gfx, values.fps_limit, tr("Guest FPS cap"),
                  tr("Limits how often supported games present; 0 follows the game"),
                  QStringLiteral("lightning"), Tints::Green());
    AddSettingRow(picture, gfx, values.resolution_setup, tr("Resolution"),
                  tr("Choose the game's internal rendering scale"), QStringLiteral("monitor"),
                  Tints::Gold());
    AddSettingRow(picture, gfx, values.scaling_filter, tr("Scaling filter"),
                  tr("How the image fits the window"), QStringLiteral("stack"), Tints::Blue());
    auto* frame_generation = SettingsPage::BindFrameGeneration(gfx);
    if (frame_generation != nullptr) {
        SettingsPage::AddRow(picture, tr("Frame generation"),
                             tr("Adds displayed frames between game frames"),
                             QStringLiteral("film-strip"), Tints::Lavender(), frame_generation);
    }
    start_fullscreen = new Spotlight::ToggleSwitch;
    start_fullscreen->setChecked(UISettings::values.fullscreen.GetValue());
    start_fullscreen->setEnabled(!system.IsPoweredOn());
    SettingsPage::AddRow(picture, tr("Fullscreen when a game starts"), {},
                         QStringLiteral("monitor"), Color::Accent(), start_fullscreen);

    int advanced_count = 0;
    const bool has_frame_gen_choices = qobject_cast<QComboBox*>(frame_generation) != nullptr;
    for (auto* tab : std::initializer_list<QWidget*>{
             graphics_tab.get(),
             graphics_advanced_tab.get(),
             graphics_extensions_tab.get(),
         }) {
        for (auto* editor : tab->findChildren<ConfigurationShared::Widget*>()) {
            const auto id = editor->SettingId();
            if (has_frame_gen_choices &&
                (id == values.frame_gen_type.Id() || id == values.frame_gen_multiplier.Id())) {
                continue;
            }
            if (editor->Valid() && editor->isVisibleTo(tab) && id != values.resolution_setup.Id() &&
                id != values.scaling_filter.Id() && id != values.frame_gen.Id() &&
                id != values.fps_limit.Id()) {
                ++advanced_count;
            }
        }
    }
    auto* footnote = new QWidget;
    auto* footnote_layout = new QHBoxLayout(footnote);
    footnote_layout->setContentsMargins(6, 0, 6, 0);
    footnote_layout->setSpacing(8);
    auto* info = new QLabel;
    info->setPixmap(Spotlight::IconPixmap(QStringLiteral("info"), 14, Color::TextSecondary(),
                                          devicePixelRatioF()));
    footnote_layout->addWidget(info, 0, Qt::AlignTop);
    auto* footnote_text = SettingsPage::Text(
        tr("VSync, speed limit and more are separate controls under Show advanced (%n more).",
           nullptr, advanced_count),
        12, 500, true);
    footnote_text->setWordWrap(true);
    footnote_layout->addWidget(footnote_text, 1);
    settings_page->AddBlock(graphics, footnote, tr("Show advanced"));
}

void ConfigureDialog::EnsureInput() {
    if (input_initialized) {
        return;
    }
    input_initialized = true;
    input_tab->Initialize(input_subsystem);
}

void ConfigureDialog::BuildControls(int category, int shortcuts) {
    EnsureInput();
    const auto pages = input_tab->GetSubTabs();
    auto* input_tabs = input_tab->findChild<QTabWidget*>(QStringLiteral("tabWidget"));
    const int mapping = settings_page->AddSubPage(category, tr("Map buttons"), {}, true);
    mapping_page = mapping;
    auto* player_pages = new QStackedWidget;
    QStringList player_names;
    QList<QCheckBox*> connected;
    const auto& players = Settings::values.players.GetValue();
    vibration_strength.clear();
    for (int i = 0; i < 8; ++i) {
        player_names.append(tr("P%1").arg(i + 1));
        auto* state = input_tab->findChild<QCheckBox*>(
            QStringLiteral("checkboxPlayer%1Connected").arg(i + 1));
        connected.append(state);
        vibration_strength.append(players[i].vibration_strength);
        auto* type = pages[i]->findChild<QComboBox*>(QStringLiteral("comboControllerType"));
        auto* profile = pages[i]->findChild<QComboBox*>(QStringLiteral("comboProfiles"));
        auto* card = SettingsPage::Card();
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(1, 1, 1, 1);
        card_layout->setSpacing(0);
        auto* header = new QWidget;
        header->setProperty("spotlightTransparent", true);
        auto* header_layout = new QHBoxLayout(header);
        header_layout->setContentsMargins(18, 18, 18, 18);
        header_layout->setSpacing(20);
        auto* picture = new QLabel;
        picture->setFixedSize(150, 110);
        picture->setAlignment(Qt::AlignCenter);
        picture->setPixmap(Spotlight::IconPixmap(QStringLiteral("game-controller"), 72,
                                                 Color::Text(), devicePixelRatioF()));
        picture->setStyleSheet(
            QStringLiteral("QLabel { background: rgba(255,255,255,20); border-radius: 24px; }"));
        header_layout->addWidget(picture, 0, Qt::AlignVCenter);
        auto* detail = new QVBoxLayout;
        detail->setSpacing(4);
        detail->addWidget(SettingsPage::Text(tr("Player %1").arg(i + 1), 13, 600, true));
        auto* controller = SettingsPage::Text(type->currentText(), 24, 600);
        controller->setFont(Spotlight::DisplayFont(24));
        connect(type, &QComboBox::currentTextChanged, controller, &QLabel::setText);
        detail->addWidget(controller);
        auto* status_row = new QHBoxLayout;
        status_row->setSpacing(7);
        auto* dot = new QLabel;
        dot->setFixedSize(8, 8);
        auto* connected_text = SettingsPage::Text({}, 13, 500, true);
        const auto sync_state = [connected_text, dot, state] {
            connected_text->setText(state->isChecked() ? tr("Connected") : tr("Disconnected"));
            const QString dot_color = state->isChecked() ? QStringLiteral("#3FD18B")
                                                         : QStringLiteral("rgba(255,255,255,70)");
            dot->setStyleSheet(
                QStringLiteral("QLabel { background: %1; border-radius: 4px; }").arg(dot_color));
        };
        sync_state();
        connect(state, &QAbstractButton::toggled, connected_text, sync_state);
        status_row->addWidget(dot, 0, Qt::AlignVCenter);
        status_row->addWidget(connected_text);
        status_row->addStretch();
        detail->addLayout(status_row);
        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 8, 0, 0);
        actions->setSpacing(8);
        auto* map_buttons = new Spotlight::PillButton(
            tr("Map buttons"), QStringLiteral("keyboard"), Spotlight::PillButton::Kind::Accent, 38);
        connect(map_buttons, &QAbstractButton::clicked, this, [this, i, mapping, input_tabs] {
            input_tabs->setCurrentIndex(i);
            settings_page->SetCategory(mapping);
        });
        actions->addWidget(map_buttons);
        auto* profiles = SettingsPage::BindCombo(
            profile, tr("Profile: none"), [](const QComboBox& combo, int index) {
                return tr("Profile: %1").arg(combo.itemText(index));
            });
        profiles->setAccessibleName(tr("Controller profile"));
        actions->addWidget(profiles);
        actions->addStretch();
        detail->addLayout(actions);
        header_layout->addLayout(detail, 1);
        card_layout->addWidget(header);

        auto* divider = new QWidget;
        divider->setFixedHeight(1);
        divider->setStyleSheet(QStringLiteral("QWidget { background: rgba(255,255,255,18); }"));
        auto* divider_row = new QHBoxLayout;
        divider_row->setContentsMargins(18, 0, 18, 0);
        divider_row->addWidget(divider);
        card_layout->addLayout(divider_row);
        auto* rows = new QVBoxLayout;
        rows->setSpacing(0);
        card_layout->addLayout(rows);
        SettingsPage::AddRow(rows, tr("Connected"), tr("Players connect in order"),
                             QStringLiteral("power"), Tints::Blue(),
                             SettingsPage::BindToggle(state));
        SettingsPage::AddRow(rows, tr("Controller type"), tr("What the game sees"),
                             QStringLiteral("game-controller"), Tints::Green(),
                             SettingsPage::BindCombo(type));
        auto* vibration = new QWidget;
        vibration->setProperty("spotlightTransparent", true);
        auto* vibration_layout = new QHBoxLayout(vibration);
        vibration_layout->setContentsMargins(0, 0, 0, 0);
        vibration_layout->setSpacing(6);
        auto* slider = new QSlider(Qt::Horizontal, vibration);
        slider->hide();
        slider->setRange(1, 150);
        slider->setValue(vibration_strength[i]);
        auto* strength = SettingsPage::Text(tr("%1%").arg(slider->value()), 13, 600);
        strength->setMinimumWidth(44);
        strength->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(slider, &QSlider::valueChanged, strength, [this, i, strength](int value) {
            vibration_strength[i] = value;
            strength->setText(tr("%1%").arg(value));
        });
        vibration_layout->addWidget(SettingsPage::BindSlider(slider));
        vibration_layout->addWidget(strength);
        SettingsPage::AddRow(rows, tr("Vibration"), tr("Rumble strength for this player"),
                             QStringLiteral("vibrate"), Tints::Gold(), vibration);
        player_pages->addWidget(card);
    }

    auto* picker = new QWidget;
    auto* picker_layout = new QHBoxLayout(picker);
    picker_layout->setContentsMargins(0, 0, 0, 0);
    auto* player_picker = new Spotlight::SegmentedControl(
        player_names, Spotlight::SegmentedControl::Kind::Accent, 38);
    player_picker->setAccessibleName(tr("Player"));
    picker_layout->addWidget(player_picker, 0, Qt::AlignLeft);
    picker_layout->addStretch();
    auto* count = SettingsPage::Text({}, 13, 500, true);
    picker_layout->addWidget(count);
    const auto sync_count = [connected, count] {
        const int active = std::count_if(connected.cbegin(), connected.cend(),
                                         [](const auto* player) { return player->isChecked(); });
        count->setText(active == 1 ? tr("1 controller connected")
                                   : tr("%1 controllers connected").arg(active));
    };
    sync_count();
    for (auto* state : connected) {
        connect(state, &QAbstractButton::toggled, count, sync_count);
    }
    connect(player_picker, &Spotlight::SegmentedControl::CurrentChanged, player_pages,
            &QStackedWidget::setCurrentIndex);
    settings_page->AddBlock(category, picker, tr("Players"));
    settings_page->AddBlock(category, player_pages, tr("Controller and profile"));

    auto* controllers = settings_page->AddSection(category, tr("All players"));
    const auto configure_pair = [](QGroupBox* group, QPushButton* configure) {
        auto* manage = SettingsPage::Manage(tr("Configure"));
        manage->setEnabled(group->isChecked());
        connect(group, &QGroupBox::toggled, manage, &QWidget::setEnabled);
        connect(manage, &QAbstractButton::clicked, configure, &QPushButton::click);
        auto* pair = new QWidget;
        pair->setProperty("spotlightTransparent", true);
        auto* layout = new QHBoxLayout(pair);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(12);
        layout->addWidget(manage);
        layout->addWidget(SettingsPage::BindToggle(group));
        return pair;
    };
    SettingsPage::AddRow(
        controllers, tr("Motion"), tr("Gyro and touch from your controllers"),
        QStringLiteral("arrows-out-cardinal"), Tints::Gold(),
        configure_pair(input_tab->findChild<QGroupBox*>(QStringLiteral("motionGroup")),
                       input_tab->findChild<QPushButton*>(QStringLiteral("motionButton"))));
    SettingsPage::AddRow(
        controllers, tr("Vibration"), tr("Rumble on or off, and per-controller tests"),
        QStringLiteral("vibrate"), Tints::Lavender(),
        configure_pair(input_tab->findChild<QGroupBox*>(QStringLiteral("vibrationGroup")),
                       input_tab->findChild<QPushButton*>(QStringLiteral("vibrationButton"))));

    auto* docked = input_tab->findChild<QRadioButton*>(QStringLiteral("radioDocked"));
    auto* handheld = input_tab->findChild<QRadioButton*>(QStringLiteral("radioUndocked"));
    auto* mode = new Spotlight::SegmentedControl({tr("Docked"), tr("Handheld")},
                                                 Spotlight::SegmentedControl::Kind::Accent, 38);
    const auto sync_mode = [mode, docked] {
        const QSignalBlocker blocker(mode);
        mode->SetCurrentIndex(docked->isChecked() ? 0 : 1);
    };
    sync_mode();
    connect(docked, &QAbstractButton::toggled, mode, sync_mode);
    connect(mode, &Spotlight::SegmentedControl::CurrentChanged, this,
            [docked, handheld, sync_mode](int index) {
                auto* source = index == 0 ? docked : handheld;
                if (source->isEnabled()) {
                    source->click();
                }
                sync_mode();
            });
    SettingsPage::AddRow(controllers, tr("Console mode"), tr("Choose docked or handheld mode"),
                         QStringLiteral("monitor"), Tints::Blue(), mode);
    QStringList hotkey_names;
    for (const auto& [action, name] : {std::pair{"Continue/Pause Emulation", tr("Pause")},
                                       std::pair{"Stop Emulation", tr("Stop")},
                                       std::pair{"Fullscreen", tr("Fullscreen")},
                                       std::pair{"Toggle Performance Overlay", tr("Overlay")}}) {
        const auto sequence = registry.GetKeySequence("Main Window", action);
        if (!sequence.isEmpty()) {
            hotkey_names.append(
                QStringLiteral("%1 %2").arg(name, sequence.toString(QKeySequence::NativeText)));
        }
    }
    auto* edit_hotkeys = SettingsPage::Manage();
    connect(edit_hotkeys, &QAbstractButton::clicked, settings_page,
            [this, shortcuts] { settings_page->SetCategory(shortcuts); });
    SettingsPage::AddRow(controllers, tr("Hotkeys"),
                         hotkey_names.isEmpty() ? tr("Edit keyboard and controller shortcuts")
                                                : hotkey_names.join(QStringLiteral(", ")),
                         QStringLiteral("keyboard"), Tints::Gold(), edit_hotkeys);

    settings_page->SetBuilder(mapping, [this, mapping, input_tabs, player_names] {
        auto* picker_row = new QWidget;
        auto* picker_row_layout = new QHBoxLayout(picker_row);
        picker_row_layout->setContentsMargins(0, 0, 0, 0);
        picker_row_layout->setSpacing(8);
        auto* players_control = new Spotlight::SegmentedControl(
            player_names, Spotlight::SegmentedControl::Kind::Accent, 38);
        players_control->SetCurrentIndex(input_tabs->currentIndex());
        connect(players_control, &Spotlight::SegmentedControl::CurrentChanged, input_tabs,
                &QTabWidget::setCurrentIndex);
        connect(input_tabs, &QTabWidget::currentChanged, players_control,
                [players_control](int index) {
                    const QSignalBlocker blocker(players_control);
                    players_control->SetCurrentIndex(index);
                });
        picker_row_layout->addWidget(players_control);
        picker_row_layout->addStretch();
        for (const auto& name : {QStringLiteral("buttonRestoreDefaults"),
                                 QStringLiteral("buttonClearAll")}) {
            picker_row_layout->addWidget(
                SettingsPage::BindButton(input_tab->findChild<QPushButton*>(name)));
        }
        settings_page->AddBlock(mapping, picker_row, tr("Players"));
        input_tabs->tabBar()->hide();
        input_tabs->setDocumentMode(true);
        input_tab->findChild<QWidget*>(QStringLiteral("consoleInputSettings"))->hide();
        for (auto* combo : input_tabs->findChildren<QComboBox*>()) {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(10);
        }
        input_tabs->setProperty("spotlightTransparent", true);
        for (auto* child : input_tabs->findChildren<QWidget*>()) {
            if (!qobject_cast<QAbstractButton*>(child) && !qobject_cast<QComboBox*>(child) &&
                !qobject_cast<QGroupBox*>(child) && !qobject_cast<QAbstractSlider*>(child)) {
                child->setAutoFillBackground(false);
                child->setProperty("spotlightTransparent", true);
            }
        }
        input_tabs->setObjectName(QStringLiteral("spotlightMapping"));
        input_tabs->setStyleSheet(MappingStyle());
        input_tabs->setMinimumHeight(560);
        const auto size_to_current = [input_tabs] {
            for (int i = 0; i < input_tabs->count(); ++i) {
                const auto policy = i == input_tabs->currentIndex() ? QSizePolicy::Preferred
                                                                    : QSizePolicy::Ignored;
                input_tabs->widget(i)->setSizePolicy(policy, policy);
            }
        };
        size_to_current();
        connect(input_tabs, &QTabWidget::currentChanged, input_tabs, size_to_current);
        settings_page->AddBlock(mapping, input_tabs, tr("Map buttons"));
    });
    auto* colours = settings_page->AddSection(category, tr("Controller colours"), true);
    for (int i = 0; i < 8; ++i) {
        auto* swatches = new QWidget;
        auto* swatch_layout = new QHBoxLayout(swatches);
        swatch_layout->setContentsMargins(0, 0, 0, 0);
        swatch_layout->setSpacing(8);
        for (const auto& [part, name] : std::array{
                 std::pair{"left_body", tr("Left body")},
                 std::pair{"left_buttons", tr("Left buttons")},
                 std::pair{"right_body", tr("Right body")},
                 std::pair{"right_buttons", tr("Right buttons")},
             }) {
            auto* source = pages[8]->findChild<QPushButton*>(
                QStringLiteral("player%1_%2_button").arg(i + 1).arg(QLatin1String(part)));
            if (source != nullptr) {
                source->setProperty("spotlightBound", true);
                swatch_layout->addWidget(new ColourSwatch(source, name));
            }
        }
        SettingsPage::AddRow(colours, tr("Player %1").arg(i + 1),
                             tr("Left body, left buttons, right body, right buttons"),
                             QStringLiteral("palette"), Tints::Lavender(), swatches);
    }
    settings_page->AddPage(category, pages[8], tr("Input devices"));
    settings_page->AddPage(category, pages[9], tr("Microphone"));
}

void ConfigureDialog::RebuildGameFolders() {
    while (auto* item = folders_layout->takeAt(0)) {
        item->widget()->hide();
        item->widget()->deleteLater();
        delete item;
    }
    int custom_count = 0;
    for (int i = 0; i < game_dirs.size(); ++i) {
        const auto& folder = game_dirs[i];
        const bool built_in = IsBuiltInFolder(folder);
        custom_count += !built_in;
        const QString path = QString::fromStdString(folder.path);
        auto* menu_button = new Spotlight::CircleButton(QStringLiteral("dots-three"), 36);
        auto* menu = new QMenu(menu_button);
        const auto changed = [this] {
            game_dirs_changed = true;
            RebuildGameFolders();
        };
        menu->addAction(tr("Open location"), this,
                        [this, path] { emit OpenGameFolderRequested(path); });
        auto* up = menu->addAction(tr("Move up"), this, [this, i, changed] {
            game_dirs.swapItemsAt(i, i - 1);
            changed();
        });
        up->setEnabled(i > 0);
        auto* down = menu->addAction(tr("Move down"), this, [this, i, changed] {
            game_dirs.swapItemsAt(i, i + 1);
            changed();
        });
        down->setEnabled(i + 1 < game_dirs.size());
        if (!built_in) {
            auto* scan = menu->addAction(tr("Scan subfolders"));
            scan->setCheckable(true);
            scan->setChecked(folder.deep_scan);
            connect(scan, &QAction::toggled, this, [this, i, changed](bool enabled) {
                game_dirs[i].deep_scan = enabled;
                changed();
            });
            menu->addAction(tr("Remove"), this, [this, i, changed] {
                game_dirs.removeAt(i);
                changed();
            });
        }
        connect(menu_button, &QAbstractButton::clicked, menu, [menu, menu_button] {
            menu->popup(menu_button->mapToGlobal(QPoint(0, menu_button->height())));
        });
        const QString title = folder.path == "SDMC"       ? tr("SD card")
                              : folder.path == "UserNAND" ? tr("User NAND")
                              : folder.path == "SysNAND"  ? tr("System NAND")
                                                          : QDir(path).dirName();
        SettingsPage::AddRow(folders_layout, title, built_in ? tr("Built-in location") : path,
                             QStringLiteral("folder"), Tints::Green(), menu_button);
    }
    auto* add = new Spotlight::PillButton(tr("Add folder"), QStringLiteral("plus"));
    connect(add, &QAbstractButton::clicked, this, &ConfigureDialog::AddGameFolder);
    SettingsPage::AddRow(folders_layout, tr("Add a game folder"), tr("Scan a folder for games"),
                         QStringLiteral("folder-open"), Tints::Green(), add);
    if (folder_summary != nullptr) {
        folder_summary->setText(GameFolderSummary());
    }
}

QString ConfigureDialog::GameFolderSummary() const {
    const auto custom = std::count_if(game_dirs.cbegin(), game_dirs.cend(),
                                      [](const auto& folder) { return !IsBuiltInFolder(folder); });
    return custom == 1 ? tr("1 folder added") : tr("%1 folders added").arg(custom);
}

void ConfigureDialog::AddGameFolder() {
    const QString path = QFileDialog::getExistingDirectory(this, tr("Select game folder"));
    const UISettings::GameDir dir{path.toStdString(), false, true};
    if (!path.isEmpty() && !game_dirs.contains(dir)) {
        game_dirs.append(dir);
        game_dirs_changed = true;
        RebuildGameFolders();
    }
}

void ConfigureDialog::OpenGetStarted(int controls, const QList<PresetCard*>& presets) {
    EnsureInput();
    auto* player = input_tab->GetSubTabs().front();
    auto* type = player->findChild<QComboBox*>(QStringLiteral("comboControllerType"));
    auto* device = player->findChild<QComboBox*>(QStringLiteral("comboDevices"));
    auto* connected = input_tab->findChild<QCheckBox*>(QStringLiteral("checkboxPlayer1Connected"));
    Spotlight::GetStarted wizard(this, system,
                                 {
                                     .game_folders =
                                         [this] {
                                             QStringList folders;
                                             for (const auto& folder : game_dirs) {
                                                 if (!IsBuiltInFolder(folder)) {
                                                     folders.append(
                                                         QString::fromStdString(folder.path));
                                                 }
                                             }
                                             return folders;
                                         },
                                     .add_game_folder = [this] { AddGameFolder(); },
                                     .controller = type->currentText(),
                                     .controller_device = device->currentText(),
                                     .controller_connected = connected->isChecked(),
                                     .presets = presets,
                                 });
    connect(&wizard, &Spotlight::GetStarted::ContentInstalled, this,
            &ConfigureDialog::ContentInstalled);
    wizard.exec();
    if (wizard.MapButtonsRequested()) {
        settings_page->SetCategory(controls);
        input_tab->findChild<QTabWidget*>(QStringLiteral("tabWidget"))->setCurrentIndex(0);
        settings_page->SetCategory(mapping_page);
    }
}
