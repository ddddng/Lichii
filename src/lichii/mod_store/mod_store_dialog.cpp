// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/mod_store/mod_store_dialog.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QTemporaryFile>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include "common/scm_rev.h"
#include "lichii/mod_store/mod_store_client.h"
#include "lichii/mod_store/mod_store_install.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace ModStore {
namespace {

constexpr int GamePageSize = 50;
constexpr int ModPageSize = 8;
constexpr qint64 MaxArchiveBytes = 64 * 1024 * 1024;
constexpr int CardColumns = 4;
constexpr qsizetype ThumbnailConcurrency = 3;
constexpr std::array Categories{ModType::Graphics, ModType::Fps, ModType::Cheats,
                                ModType::Translation, ModType::Gameplay};
using Spotlight::CircleButton;
using Spotlight::PillButton;
using Spotlight::SegmentedControl;
using Spotlight::ToggleSwitch;
namespace Color = Spotlight::Color;

enum class Compatibility { Match, Unknown, Mismatch };

std::optional<qulonglong> BuildOrdinal(const QString& tag) {
    static const QRegularExpression format(QStringLiteral("^build-[1-9][0-9]*$"));
    bool ok = false;
    const auto number = tag.mid(6).toULongLong(&ok);
    if (!ok || !format.match(tag).hasMatch()) {
        return std::nullopt;
    }
    return number;
}

bool KnownBuild(const QString& build) {
    static const QRegularExpression format(QStringLiteral("^[0-9A-Fa-f]{64}$"));
    return format.match(build).hasMatch() &&
           std::ranges::any_of(build, [](QChar digit) { return digit != QLatin1Char('0'); });
}

Compatibility MatchVersion(const QString& selected_title, const GameIdentity& game,
                           const Version& version) {
    if (selected_title != version.title_id ||
        (!game.display_version.isEmpty() && game.display_version != version.game_version) ||
        (KnownBuild(game.build_id) &&
         game.build_id.compare(version.build_id, Qt::CaseInsensitive) != 0)) {
        return Compatibility::Mismatch;
    }
    const auto running = BuildOrdinal(QString::fromLatin1(Common::g_build_release_tag));
    const auto required = BuildOrdinal(version.min_lichii_build);
    if (running && required && *running < *required) {
        return Compatibility::Mismatch;
    }
    return game.installed && !game.display_version.isEmpty() && KnownBuild(game.build_id) &&
                   running && required
               ? Compatibility::Match
               : Compatibility::Unknown;
}

Compatibility MatchGitHubBuild(const GameIdentity& game, const GitHubBuild& build) {
    if (!game.installed || !KnownBuild(game.build_id)) {
        return Compatibility::Unknown;
    }
    if (build.main) {
        return game.build_id.startsWith(build.id, Qt::CaseInsensitive) ? Compatibility::Match
                                                                      : Compatibility::Mismatch;
    }
    return build.id.size() == 64 && game.build_id.compare(build.id, Qt::CaseInsensitive) == 0
               ? Compatibility::Match
               : Compatibility::Unknown;
}

struct Offer {
    std::variant<Version, ExternalFile, GitHubBuild> file;
    QString version;
    Compatibility compatibility;
    bool supported;
};

QString ExternalVersionPrefix(qint64 id) {
    return QStringLiteral("%1-").arg(id);
}

QString ExternalVersion(qint64 id, const QByteArray& sha256) {
    return ExternalVersionPrefix(id) + QString::fromLatin1(sha256.toHex().left(16));
}

QString CssColor(const QColor& color) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QLabel* PlainLabel(QWidget* parent, bool wrap = false, const QColor& color = Color::Text(),
                   int size = 13, int weight = QFont::Normal) {
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(wrap);
    label->setFont(Spotlight::Font(size, weight));
    label->setStyleSheet(
        QStringLiteral("color: %1; background: transparent;").arg(CssColor(color)));
    return label;
}

void PaintImage(QPainter& painter, const QRect& bounds, const QImage& image, int radius,
                bool crop) {
    QPainterPath clip;
    clip.addRoundedRect(bounds, radius, radius);
    painter.fillPath(clip, Color::PanelDeep());
    if (image.isNull()) {
        const auto size = qBound(14, bounds.height() / 4, 32);
        painter.drawPixmap(bounds.center() - QPoint(size / 2, size / 2),
                           Spotlight::IconPixmap(u"image", size, Color::TextFaint(),
                                                 painter.device()->devicePixelRatioF()));
        return;
    }
    const QSize size = image.size().scaled(bounds.size(), crop ? Qt::KeepAspectRatioByExpanding
                                                               : Qt::KeepAspectRatio);
    QRect image_rect(QPoint{}, size);
    image_rect.moveCenter(bounds.center());
    painter.save();
    painter.setClipPath(clip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(image_rect, image);
    painter.restore();
}

QIcon RoundedGameIcon(const QIcon& icon) {
    if (icon.isNull()) {
        return Spotlight::Icon(u"game-controller", Color::TextSecondary(), 24);
    }
    QIcon rounded;
    for (const qreal ratio : {1.0, 1.5, 2.0}) {
        const int size = qRound(24 * ratio);
        QPixmap square(size, size);
        square.setDevicePixelRatio(ratio);
        square.fill(Qt::transparent);
        {
            QPainter painter(&square);
            painter.setRenderHint(QPainter::Antialiasing);
            PaintImage(painter, QRect(0, 0, 24, 24), icon.pixmap(QSize(24, 24), ratio).toImage(), 3,
                       false);
        }
        rounded.addPixmap(square);
    }
    return rounded;
}

class PreviewImage final : public QWidget {
public:
    PreviewImage(int radius, bool crop, QWidget* parent)
        : QWidget(parent), radius(radius), crop(crop) {
        setObjectName(QStringLiteral("storeContents"));
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    QSize sizeHint() const override {
        return {600, heightForWidth(600)};
    }

    int heightForWidth(int width) const override {
        return qRound(width * 9.0 / 16);
    }

    void SetImage(QImage value) {
        image = std::move(value);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        PaintImage(painter, rect(), image, radius, crop);
    }

private:
    QImage image;
    int radius;
    bool crop;
};

class PreviewButton final : public QAbstractButton {
public:
    explicit PreviewButton(QWidget* parent) : QAbstractButton(parent) {
        setFixedSize(76, 44);
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
    }

    void SetImage(QImage value) {
        image = std::move(value);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (hasFocus() || isChecked() || underMouse()) {
            painter.setPen(QPen(hasFocus() ? Color::Text() : Color::Accent(), 2));
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 9, 9);
        }
        PaintImage(painter, rect().adjusted(2, 2, -2, -2), image, Spotlight::Radius::KeyCap, true);
    }

private:
    QImage image;
};

class ModCard final : public QAbstractButton {
public:
    ModCard(const QString& title, QString subtitle, QString source, QString verification,
            QWidget* parent)
        : QAbstractButton(parent), subtitle(std::move(subtitle)), source(std::move(source)),
          verification(std::move(verification)) {
        setText(title);
        setAccessibleName(title);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setMinimumWidth(160);
    }

    QSize sizeHint() const override {
        return {290, heightForWidth(290)};
    }

    int heightForWidth(int width) const override {
        return qRound((width - 16) * 9.0 / 16) + 94;
    }

    void SetImage(QImage value) {
        image = std::move(value);
        update();
    }

    const QImage& Image() const {
        return image;
    }

    void SetInstalled(QString label, bool enabled) {
        installed = std::move(label);
        installed_enabled = enabled;
        setAccessibleDescription(
            QStringList{subtitle, source, installed, verification}.join(u'\n'));
        update();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            click();
            return;
        }
        QAbstractButton::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(isEnabled() ? 1.0 : 0.5);
        painter.setPen(hasFocus()     ? Color::Accent()
                       : underMouse() ? Color::Stroke()
                                      : Color::StrokeSubtle());
        painter.setBrush(underMouse() || isDown() ? Color::Fill() : Color::FillFaint());
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                Spotlight::Radius::Card, Spotlight::Radius::Card);
        const QRect illustration(8, 8, width() - 16, height() - 94);
        PaintImage(painter, illustration, image, 10, true);
        const int title_top = illustration.bottom() + 10;
        const int content_width = width() - 28;
        const auto mark = [&](QStringView icon, const QString& text, const QColor& color, int size,
                              int weight, int top, bool right, int available) {
            painter.setFont(Spotlight::Font(size, weight));
            const auto label =
                painter.fontMetrics().elidedText(text, Qt::ElideRight, available - 18);
            const int text_width = painter.fontMetrics().horizontalAdvance(label);
            const int left = right ? width() - 14 - text_width - 18 : 14;
            painter.drawPixmap(left, top + 2,
                               Spotlight::IconPixmap(icon, 14, color, devicePixelRatioF()));
            painter.setPen(color);
            painter.drawText(QRect(left + 18, top, text_width, 18), Qt::AlignVCenter, label);
            return text_width + 18;
        };
        const int badge_width =
            installed.isEmpty()
                ? 0
                : mark(u"check-circle-fill", installed,
                       installed_enabled ? Color::Success() : Color::TextSecondary(), 11,
                       QFont::DemiBold, title_top + 1, true, content_width / 2) +
                      8;
        painter.setFont(Spotlight::Font(15, QFont::DemiBold));
        painter.setPen(Color::Text());
        const QRect title_rect(14, title_top, content_width - badge_width, 20);
        painter.drawText(
            title_rect, Qt::AlignVCenter,
            painter.fontMetrics().elidedText(text(), Qt::ElideRight, title_rect.width()));
        painter.setFont(Spotlight::Font(12, QFont::Normal));
        painter.setPen(Color::TextSecondary());
        painter.drawText(QRect(14, title_top + 22, content_width, 16), Qt::AlignVCenter,
                         painter.fontMetrics().elidedText(subtitle, Qt::ElideRight, content_width));
        const int verification_width =
            verification.isEmpty()
                ? 0
                : mark(u"warning-circle", verification, Color::Warning(), 12, QFont::DemiBold,
                       title_top + 43, true, content_width * 3 / 5) +
                      8;
        mark(u"globe", source, Color::TextSecondary(), 12, QFont::Medium, title_top + 43, false,
             content_width - verification_width);
    }

private:
    QString subtitle;
    QString source;
    QString verification;
    QString installed;
    QImage image;
    bool installed_enabled{};
};

class Dialog final : public QDialog {
    Q_DECLARE_TR_FUNCTIONS(ModStore::Dialog)

public:
    Dialog(QWidget* parent, DialogContext ctx)
        : QDialog(parent), context(std::move(ctx)), client(context.base_url, this) {
        setObjectName(QStringLiteral("spotlightModStore"));
        setWindowTitle(tr("Mod Store"));
        Spotlight::FadeInOnShow(this);
        setFont(Spotlight::Font(13, QFont::Normal));
        resize(1280, 800);
        setMinimumSize(950, 520);
        setStyleSheet(
            QStringLiteral(
                "QDialog#spotlightModStore { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
                " stop:0 %1, stop:1 %2); }"
                "QDialog#spotlightModStore QScrollArea { background: transparent; border: none; }"
                "QDialog#spotlightModStore QWidget#storeContents { background: transparent; }"
                "QComboBox[storeControl=\"true\"], QLineEdit#storeSearch {"
                " background: %3; color: %4; border: 1px solid transparent; border-radius: 18px;"
                " min-height: 34px; padding: 0 12px; }"
                "QComboBox[storeControl=\"true\"] { padding-right: 8px; }"
                "QComboBox[storeControl=\"true\"]::drop-down { width: 30px; border: none; }"
                "QComboBox[storeControl=\"true\"]::down-arrow {"
                " image: url(:/spotlight/icons/caret-down.png); width: 14px; height: 14px; }"
                "QComboBox[storeControl=\"true\"] QAbstractItemView { background: %2; color: %4;"
                " selection-background-color: %5; selection-color: %4; }"
                "QComboBox[storeControl=\"true\"]:focus, QLineEdit#storeSearch:focus { "
                "border-color: %6; }"
                "QToolButton#storeDescription { background: transparent; border: 1px solid "
                "transparent;"
                " border-radius: 3px; color: %7; padding: 0; }"
                "QToolButton#storeDescription:focus, QToolButton#storeDescription:hover { "
                "border-color: %6; }"
                "QPushButton#storeRowTitle { background: transparent; border: 1px solid "
                "transparent;"
                " border-radius: 3px; color: %4; padding: 0; text-align: left; }"
                "QPushButton#storeRowTitle:focus, QPushButton#storeRowTitle:hover { border-color: "
                "%6; }"
                "QToolButton#storeRowMenu::menu-indicator { image: none; }"
                "QFrame#storePanel { background: %8; border: 1px solid %3; border-radius: 16px; }"
                "QFrame#storeRule { background: %3; }")
                .arg(CssColor(Color::Window()), CssColor(Color::Panel()), CssColor(Color::Fill()),
                     CssColor(Color::Text()), CssColor(Color::AccentSoft()),
                     CssColor(Color::Accent()), CssColor(Color::TextSecondary()),
                     CssColor(Color::FillFaint())));
        auto palette = this->palette();
        palette.setColor(QPalette::Window, Color::Panel());
        palette.setColor(QPalette::WindowText, Color::Text());
        palette.setColor(QPalette::Base, Color::PanelDeep());
        palette.setColor(QPalette::Text, Color::Text());
        palette.setColor(QPalette::PlaceholderText, Color::TextSecondary());
        palette.setColor(QPalette::Highlight, Color::Accent());
        palette.setColor(QPalette::HighlightedText, Color::OnAccent());
        setPalette(palette);

        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto* header = new QWidget(this);
        header->setObjectName(QStringLiteral("storeContents"));
        header->setFixedHeight(64);
        auto* header_layout = new QHBoxLayout(header);
        header_layout->setContentsMargins(20, 0, 20, 0);
        header_layout->setSpacing(10);
        back = new CircleButton(QStringLiteral("caret-left"), 34, header);
        back->setAccessibleName(tr("Back"));
        back->setToolTip(tr("Back"));
        auto* heading = PlainLabel(header);
        heading->setFont(Spotlight::DisplayFont(22));
        heading->setText(tr("Mod Store"));
        game_picker = StoreCombo(header);
        game_picker->setAccessibleName(tr("Game"));
        game_picker->setIconSize({24, 24});
        game_picker->setMinimumContentsLength(9);
        game_picker->setMaximumWidth(290);
        installed_mode = new PillButton(tr("Installed"), QStringLiteral("archive"),
                                        PillButton::Kind::Control, 36, header);
        installed_mode->setCheckable(true);
        refresh = new CircleButton(QStringLiteral("arrow-clockwise"), 34, header);
        refresh->setAccessibleName(tr("Refresh catalog"));
        refresh->setToolTip(tr("Refresh catalog"));
        search = new QLineEdit(header);
        search->setObjectName(QStringLiteral("storeSearch"));
        search->setAccessibleName(tr("Search mods"));
        search->setPlaceholderText(tr("Name, author or description"));
        search->setClearButtonEnabled(true);
        search->setMaxLength(128);
        search->setMinimumWidth(170);
        search->setMaximumWidth(300);
        search->addAction(Spotlight::Icon(u"magnifying-glass", Color::TextSecondary(), 16),
                          QLineEdit::LeadingPosition);
        new QShortcut(QKeySequence::Find, this, [this] {
            if (search->isEnabled()) {
                search->setFocus();
                search->selectAll();
            }
        });
        new QShortcut(QKeySequence::Refresh, this, [this] { refresh->click(); });
        new QShortcut(QKeySequence::Back, this, [this] { back->click(); });
        header_layout->addWidget(back);
        header_layout->addWidget(heading);
        header_layout->addSpacing(16);
        header_layout->addWidget(game_picker);
        header_layout->addStretch(1);
        header_layout->addWidget(search, 1);
        header_layout->addWidget(refresh);
        header_layout->addWidget(installed_mode);
        root->addWidget(header);

        pages = new QStackedWidget(this);
        connect(pages, &QStackedWidget::currentChanged, this,
                [this] { Spotlight::FadeIn(pages->currentWidget()); });
        pages->setObjectName(QStringLiteral("storeContents"));
        BuildBrowsePage();
        BuildInstalledPage();
        BuildDetailPage();
        root->addWidget(pages, 1);
        status = PlainLabel(this, true, Color::TextSecondary(), 12);
        status->setContentsMargins(32, 0, 32, 16);
        status->hide();
        root->addWidget(status);

        progress = new QDialog(this);
        progress->setWindowTitle(tr("Installing mod"));
        progress->setWindowModality(Qt::WindowModal);
        progress->setMinimumWidth(360);
        progress->setAutoFillBackground(true);
        auto* progress_layout = new QVBoxLayout(progress);
        progress_layout->setContentsMargins(16, 16, 16, 16);
        progress_layout->setSpacing(8);
        progress_label = PlainLabel(progress, true, Color::Text(), 13, QFont::DemiBold);
        progress_catalog_note = PlainLabel(progress, true, Color::Warning(), 12);
        progress_catalog_note->setText(catalog_note->text());
        progress_bar = new QProgressBar(progress);
        progress_bar->setTextVisible(false);
        progress_bar->setFixedHeight(8);
        progress_bar->setStyleSheet(
            QStringLiteral("QProgressBar { border: none; background: %1; border-radius: 4px; }"
                           "QProgressBar::chunk { background: %2; border-radius: 4px; }")
                .arg(CssColor(Color::Control()), CssColor(Color::Accent())));
        cancel_button = new PillButton(tr("Cancel"), {}, PillButton::Kind::Control, 32, progress);
        progress_layout->addWidget(progress_label);
        progress_layout->addWidget(progress_catalog_note);
        progress_layout->addWidget(progress_bar);
        progress_layout->addWidget(cancel_button, 0, Qt::AlignRight);
        connect(cancel_button, &QAbstractButton::clicked, this, [this] { CancelDownload(); });
        connect(progress, &QDialog::rejected, this, [this] { CancelDownload(); });

        debounce.setSingleShot(true);
        debounce.setInterval(240);
        connect(&debounce, &QTimer::timeout, this, [this] { ReloadMods(); });
        connect(search, &QLineEdit::textChanged, this, [this] {
            more->hide();
            debounce.start();
        });
        const auto filters_changed = [this] { ReloadMods(); };
        connect(type, &SegmentedControl::CurrentChanged, this, filters_changed);
        connect(sort, &QComboBox::currentIndexChanged, this, filters_changed);
        connect(refresh, &QAbstractButton::clicked, this, [this] {
            identities.clear();
            if (!installed_mode->isChecked()) {
                LoadGames();
            }
            ReloadMods();
        });
        connect(installed_mode, &QAbstractButton::toggled, this, [this](bool installed) {
            installed_mode->SetKind(installed ? PillButton::Kind::Accent
                                              : PillButton::Kind::Control);
            ReloadMods();
        });
        connect(game_picker, &QComboBox::currentIndexChanged, this,
                [this] { SelectGame(game_picker->currentData().toString()); });
        connect(more, &QAbstractButton::clicked, this, [this] {
            if (next && !debounce.isActive() && !reload_after_install) {
                FetchMods(*next, list_epoch);
            }
        });
        connect(back, &QAbstractButton::clicked, this, [this] { NavigateBack(); });

        title = context.initial_title_id;
        PopulateGames();
        ClearDetail();
        QTimer::singleShot(0, this, [this] {
            LoadGames();
            ReloadMods();
        });
    }

    bool Changed() const {
        return changed;
    }

    void reject() override {
        if (applying) {
            return;
        }
        if (busy) {
            CancelDownload();
            return;
        }
        QDialog::reject();
    }

private:
    void NavigateBack() {
        if (pages->currentWidget() != detail_page) {
            reject();
            return;
        }
        const auto id = local_id;
        const auto view_title = local_title;
        ClearDetail();
        RestoreListPage();
        if (installed_mode->isChecked()) {
            ShowInstalled();
            FocusInstalledRow(id, view_title);
        } else if (auto* card = cards.value(id)) {
            card->setFocus();
        }
    }

    static QComboBox* StoreCombo(QWidget* parent) {
        auto* combo = new QComboBox(parent);
        combo->setProperty("storeControl", true);
        combo->setFont(Spotlight::Font(13, QFont::DemiBold));
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        return combo;
    }

    static QScrollArea* ScrollPage(QWidget* contents) {
        auto* scroll = new QScrollArea;
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(contents);
        scroll->viewport()->setObjectName(QStringLiteral("storeContents"));
        scroll->viewport()->setAutoFillBackground(false);
        contents->setAutoFillBackground(false);
        contents->setObjectName(QStringLiteral("storeContents"));
        return scroll;
    }

    void BuildBrowsePage() {
        browse_page = new QWidget(pages);
        browse_page->setObjectName(QStringLiteral("storeContents"));
        auto* body = new QVBoxLayout(browse_page);
        body->setContentsMargins(32, 6, 32, 0);
        body->setSpacing(16);
        auto* tools = new QHBoxLayout;
        tools->setSpacing(12);
        QStringList categories{tr("All types")};
        for (const auto category : Categories) {
            categories.append(TypeName(category));
        }
        type = new SegmentedControl(categories, SegmentedControl::Kind::Accent, 38, browse_page);
        type->setAccessibleName(tr("Mod category"));
        count = PlainLabel(browse_page, false, Color::TextSecondary());
        sort = StoreCombo(browse_page);
        sort->setAccessibleName(tr("Sort mods"));
        sort->setIconSize({16, 16});
        const auto sort_icon = Spotlight::Icon(u"sort-ascending", Color::Text(), 16);
        sort->addItem(sort_icon, tr("Recently updated"), static_cast<int>(Sort::Updated));
        sort->addItem(sort_icon, tr("Name A–Z"), static_cast<int>(Sort::Name));
        sort->setFixedWidth(qMax(sort->fontMetrics().horizontalAdvance(sort->itemText(0)),
                                 sort->fontMetrics().horizontalAdvance(sort->itemText(1))) +
                            76);
        tools->addWidget(type);
        tools->addStretch();
        tools->addWidget(count);
        tools->addWidget(sort);
        body->addLayout(tools);

        auto* contents = new QWidget;
        auto* list = new QVBoxLayout(contents);
        list->setContentsMargins(0, 0, 0, 24);
        list->setSpacing(16);
        browse_message = PlainLabel(contents, true, Color::TextSecondary(), 14);
        browse_message->setAlignment(Qt::AlignCenter);
        browse_message->setMinimumHeight(140);
        list->addWidget(browse_message);
        auto* grid_host = new QWidget(contents);
        grid_host->setObjectName(QStringLiteral("storeContents"));
        card_grid = new QGridLayout(grid_host);
        card_grid->setContentsMargins(0, 0, 0, 0);
        card_grid->setSpacing(18);
        for (int i = 0; i < CardColumns; ++i) {
            card_grid->setColumnStretch(i, 1);
        }
        list->addWidget(grid_host);
        more = new PillButton(tr("Load more"), {}, PillButton::Kind::Control, 38, contents);
        more->hide();
        list->addWidget(more, 0, Qt::AlignHCenter);
        list->addStretch();
        browse_scroll = ScrollPage(contents);
        body->addWidget(browse_scroll, 1);
        pages->addWidget(browse_page);
    }

    void BuildInstalledPage() {
        auto* contents = new QWidget;
        auto* body = new QVBoxLayout(contents);
        body->setContentsMargins(40, 6, 40, 24);
        body->setSpacing(12);
        auto* heading = PlainLabel(contents);
        heading->setText(tr("Installed mods"));
        heading->setFont(Spotlight::DisplayFont(28));
        auto* subtitle = PlainLabel(contents, true, Color::TextSecondary(), 14);
        subtitle->setText(tr("Turn mods on or off. Changes apply on the next launch."));
        installed_count = PlainLabel(contents, false, Color::TextSecondary(), 13, QFont::DemiBold);
        installed_count->setContentsMargins(6, 8, 0, 0);
        auto* panel = new QFrame(contents);
        panel->setObjectName(QStringLiteral("storePanel"));
        installed_list = new QVBoxLayout(panel);
        installed_list->setContentsMargins(12, 0, 12, 0);
        installed_list->setSpacing(0);
        auto* return_to_store =
            new PillButton(tr("Browse the Mod Store"), QStringLiteral("storefront"),
                           PillButton::Kind::Accent, 36, contents);
        connect(return_to_store, &QAbstractButton::clicked, this,
                [this] { installed_mode->setChecked(false); });
        auto* actions = new QHBoxLayout;
        actions->setSpacing(14);
        actions->addWidget(return_to_store);
        auto* notice = PlainLabel(contents, true, Color::TextSecondary(), 12);
        notice->setText(tr("Installed mods are available offline."));
        actions->addWidget(notice, 1);
        body->addWidget(heading);
        body->addWidget(subtitle);
        body->addWidget(installed_count);
        body->addWidget(panel);
        body->addLayout(actions);
        body->addStretch();
        installed_page = ScrollPage(contents);
        pages->addWidget(installed_page);
    }

    void BuildDetailPage() {
        auto* contents = new QWidget;
        auto* body = new QHBoxLayout(contents);
        body->setContentsMargins(40, 8, 80, 24);
        body->setSpacing(36);
        gallery = new QWidget(contents);
        gallery->setObjectName(QStringLiteral("storeContents"));
        auto* media = new QVBoxLayout(gallery);
        media->setContentsMargins(0, 0, 0, 0);
        media->setSpacing(12);
        preview = new PreviewImage(Spotlight::Radius::Pill, false, gallery);
        preview->setAccessibleName(tr("Mod preview"));
        media->addWidget(preview);

        auto* strip = new QWidget(gallery);
        auto* strip_layout = new QHBoxLayout(strip);
        strip_layout->setSpacing(8);
        strip_layout->setContentsMargins(2, 1, 2, 1);
        thumbnails = strip_layout;
        preview_strip = ScrollPage(strip);
        preview_strip->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        preview_strip->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        preview_strip->setFixedHeight(46 + style()->pixelMetric(QStyle::PM_ScrollBarExtent));
        auto* strip_row = new QHBoxLayout;
        strip_row->setSpacing(8);
        strip_row->addWidget(preview_strip, 1);
        preview_count = PlainLabel(gallery, false, Color::TextSecondary(), 12);
        preview_count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        strip_row->addWidget(preview_count);
        media->addLayout(strip_row);
        media->addStretch();
        body->addWidget(gallery, 600);

        auto* pane = new QWidget(contents);
        pane->setObjectName(QStringLiteral("storeContents"));
        auto* detail = new QVBoxLayout(pane);
        detail->setContentsMargins(0, 0, 0, 0);
        detail->setSpacing(8);
        name = PlainLabel(pane, true);
        name->setFont(Spotlight::DisplayFont(24));
        byline = PlainLabel(pane, true, Color::TextSecondary(), 13, QFont::Medium);
        source_info = PlainLabel(pane, false, Color::TextSecondary(), 12, QFont::Medium);
        verification = PlainLabel(pane, true, Color::Warning(), 12, QFont::DemiBold);
        auto* source_icon = PlainLabel(pane);
        source_icon->setPixmap(
            Spotlight::IconPixmap(u"globe", 14, Color::TextSecondary(), devicePixelRatioF()));
        source_info_row = new QWidget(pane);
        source_info_row->setObjectName(QStringLiteral("storeContents"));
        auto* tags = new QHBoxLayout(source_info_row);
        tags->setContentsMargins(0, 0, 0, 0);
        tags->setSpacing(6);
        tags->addWidget(source_icon);
        tags->addWidget(source_info);
        tags->addSpacing(8);
        tags->addWidget(verification);
        tags->addStretch();
        detail->addWidget(name);
        detail->addWidget(byline);
        detail->addWidget(source_info_row);

        mod_actions = new QHBoxLayout;
        mod_actions->setSpacing(8);
        install = new PillButton(tr("Install"), QStringLiteral("download-simple"),
                                 PillButton::Kind::Accent, 36, pane);
        source = new PillButton({}, QStringLiteral("globe"), PillButton::Kind::Control, 36, pane);
        mod_actions->addWidget(install);
        mod_actions->addWidget(source);
        mod_actions->addStretch();
        detail->addLayout(mod_actions);

        auto* installed_actions = new QHBoxLayout;
        installed_actions->setSpacing(8);
        enable = new PillButton({}, {}, PillButton::Kind::Control, 32, pane);
        uninstall = new PillButton(tr("Uninstall"), QStringLiteral("trash"),
                                   PillButton::Kind::Control, 32, pane);
        installed_actions->addWidget(enable);
        installed_actions->addWidget(uninstall);
        installed_actions->addStretch();
        detail->addLayout(installed_actions);
        hint = PlainLabel(pane, true, Color::TextSecondary(), 12);
        detail->addWidget(hint);

        description = PlainLabel(pane, true);
        description->setTextInteractionFlags(Qt::TextSelectableByMouse);
        description_toggle = new QToolButton(pane);
        description_toggle->setObjectName(QStringLiteral("storeDescription"));
        description_toggle->setFont(Spotlight::Font(12, QFont::DemiBold));
        description_toggle->setCheckable(true);
        description_toggle->setCursor(Qt::PointingHandCursor);
        detail->addWidget(description);
        detail->addWidget(description_toggle, 0, Qt::AlignLeft);
        target_picker = StoreCombo(pane);
        target_picker->setAccessibleName(tr("Target game"));
        release = StoreCombo(pane);
        release->setAccessibleName(tr("Release or file"));
        detail->addWidget(target_picker);
        detail->addWidget(release);

        compatibility_card = new QFrame(pane);
        compatibility_card->setObjectName(QStringLiteral("storePanel"));
        auto* compatibility_layout = new QGridLayout(compatibility_card);
        compatibility_layout->setContentsMargins(12, 10, 12, 10);
        compatibility_layout->setHorizontalSpacing(10);
        compatibility_layout->setVerticalSpacing(5);
        compatibility_layout->setColumnMinimumWidth(0, 140);
        compatibility_layout->setColumnStretch(1, 1);
        auto* caption = PlainLabel(compatibility_card, false, Color::Text(), 13, QFont::DemiBold);
        caption->setText(tr("Compatibility"));
        compatibility_state =
            PlainLabel(compatibility_card, false, Color::TextSecondary(), 12, QFont::DemiBold);
        compatibility_layout->addWidget(caption, 0, 0);
        compatibility_layout->addWidget(compatibility_state, 0, 1, Qt::AlignRight);
        auto* rule = new QFrame(compatibility_card);
        rule->setObjectName(QStringLiteral("storeRule"));
        rule->setFixedHeight(1);
        compatibility_layout->addWidget(rule, 1, 0, 1, 2);
        for (size_t i = 0; i < compatibility_rows.size(); ++i) {
            auto& [key, value] = compatibility_rows[i];
            key = PlainLabel(compatibility_card, true, Color::TextSecondary(), 12);
            value = PlainLabel(compatibility_card, true, Color::Text(), 12, QFont::DemiBold);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            const auto row = static_cast<int>(i) + 2;
            compatibility_layout->addWidget(key, row, 0, Qt::AlignTop);
            compatibility_layout->addWidget(value, row, 1, Qt::AlignTop);
        }
        const auto note_row = static_cast<int>(compatibility_rows.size()) + 2;
        catalog_note = PlainLabel(compatibility_card, true, Color::Warning(), 12);
        catalog_note->setText(tr("Not verified by the catalog"));
        compatibility_layout->addWidget(catalog_note, note_row, 0, 1, 2);
        github_provenance = PlainLabel(compatibility_card, true, Color::TextSecondary(), 12);
        compatibility_layout->addWidget(github_provenance, note_row + 1, 0, 1, 2);
        detail->addWidget(compatibility_card);
        detail->addStretch();
        body->addWidget(pane, 520);
        detail_page = ScrollPage(contents);
        pages->addWidget(detail_page);

        connect(release, &QComboBox::currentIndexChanged, this, [this] { UpdateOffer(); });
        connect(target_picker, &QComboBox::currentIndexChanged, this, [this] {
            local_title = target_picker->currentData().toString();
            ReadLocal();
            BuildOffers();
        });
        connect(description_toggle, &QToolButton::toggled, this, [this](bool expanded) {
            if (selected) {
                description_toggle->setText(expanded ? tr("Show less")
                                                     : tr("Show full description"));
                description->setText(expanded ? selected->description : description_excerpt);
            }
        });
        connect(source, &QAbstractButton::clicked, this, [this] { OpenSource(); });
        connect(install, &QAbstractButton::clicked, this, [this] { StartInstall(); });
        connect(enable, &QAbstractButton::clicked, this, [this] { ToggleEnabled(); });
        connect(uninstall, &QAbstractButton::clicked, this, [this] { RemoveInstalled(); });
    }

    static QString TypeName(ModType value) {
        switch (value) {
        case ModType::Graphics:
            return tr("Graphics");
        case ModType::Fps:
            return tr("FPS");
        case ModType::Cheats:
            return tr("Cheats");
        case ModType::Translation:
            return tr("Translation");
        case ModType::Gameplay:
            return tr("Gameplay");
        }
        return {};
    }

    static QString CompatibilityName(Compatibility value) {
        switch (value) {
        case Compatibility::Match:
            return tr("Match");
        case Compatibility::Unknown:
            return tr("Unverified");
        case Compatibility::Mismatch:
            return tr("Mismatch");
        }
        return {};
    }

    void Status(const QString& text) {
        status->setText(text);
        status->setVisible(!text.isEmpty());
    }

    GameIdentity Identity(const QString& title_id) {
        const auto found = identities.constFind(title_id);
        if (found != identities.cend()) {
            return found.value();
        }
        const auto identity = !title_id.isEmpty() && context.game_identity
                                  ? context.game_identity(title_id)
                                  : GameIdentity{};
        identities.insert(title_id, identity);
        return identity;
    }

    QString GameName(const QString& id) const {
        const auto local_game =
            std::ranges::find(context.library_games, id, &LibraryGame::title_id);
        if (local_game != context.library_games.end()) {
            return local_game->name;
        }
        const auto game = std::ranges::find(games, id, &Game::title_id);
        return game == games.end() ? id : game->name;
    }

    void PopulateGames() {
        const QSignalBlocker block_picker(game_picker);
        game_picker->clear();
        game_picker->addItem(Spotlight::Icon(u"game-controller", Color::TextSecondary(), 24),
                             tr("All games"), QString{});
        for (const auto& game : context.library_games) {
            if (game.title_id.isEmpty() || game_picker->findData(game.title_id) >= 0) {
                continue;
            }
            game_picker->addItem(RoundedGameIcon(game.icon), game.name, game.title_id);
        }
        for (const auto& game : games) {
            int index = game_picker->findData(game.title_id);
            if (index < 0) {
                game_picker->addItem(game.name, game.title_id);
                index = game_picker->count() - 1;
            }
            game_picker->setItemData(index, tr("%n mod(s)", nullptr, game.mod_count),
                                     Qt::ToolTipRole);
        }
        if (!title.isEmpty() && game_picker->findData(title) < 0) {
            game_picker->addItem(title, title);
        }
        game_picker->setCurrentIndex(game_picker->findData(title));
        UpdateGamePicker();
    }

    void UpdateGamePicker() {
        const auto name = game_picker->currentText();
        const auto mods = game_picker->currentData(Qt::ToolTipRole).toString();
        const auto text_width = game_picker->fontMetrics().horizontalAdvance(name);
        game_picker->setFixedWidth(qBound(116, text_width + 90, 290));
        game_picker->setToolTip(mods.isEmpty() ? name : tr("%1 · %2").arg(name, mods));
    }

    void LoadGames(int offset = 0) {
        if (offset == 0) {
            if (games_request) {
                games_request->Cancel();
            }
            ++game_epoch;
            incoming_games.clear();
        }
        const auto epoch = game_epoch;
        games_request = client.GetGames(offset, GamePageSize, [this, epoch, offset](auto response) {
            if (epoch != game_epoch) {
                return;
            }
            if (const auto* page = std::get_if<Page<Game>>(&response)) {
                incoming_games.append(page->items);
                if (page->next_offset && *page->next_offset > offset &&
                    incoming_games.size() < 1000) {
                    LoadGames(*page->next_offset);
                    return;
                }
                games = std::move(incoming_games);
                PopulateGames();
            }
        });
    }

    void SelectGame(const QString& id) {
        if (title == id) {
            return;
        }
        title = id;
        UpdateGamePicker();
        ReloadMods();
    }

    void RestoreListPage() {
        pages->setCurrentWidget(installed_mode->isChecked() ? installed_page : browse_page);
        back->setAccessibleName(tr("Close Mod Store"));
        back->setToolTip(tr("Close Mod Store"));
    }

    void ReloadMods() {
        debounce.stop();
        ++list_epoch;
        if (list_request) {
            list_request->Cancel();
        }
        for (const auto& request : thumbnail_requests) {
            if (request) {
                request->Cancel();
            }
        }
        thumbnail_requests.clear();
        thumbnail_queue.clear();
        next.reset();
        more->hide();
        ClearDetail();
        RestoreListPage();
        Status({});
        if (installed_mode->isChecked()) {
            ShowInstalled();
            return;
        }
        qDeleteAll(cards);
        cards.clear();
        catalog.clear();
        installed_by_title.clear();
        browse_scroll->verticalScrollBar()->setValue(0);
        count->clear();
        const int category = type->CurrentIndex();
        list_filter = {title, search->text().trimmed(),
                       category > 0 ? std::optional{Categories[category - 1]} : std::nullopt,
                       static_cast<Sort>(sort->currentData().toInt())};
        FetchMods(0, list_epoch);
    }

    static QString SourceName(const Mod& mod) {
        switch (mod.source) {
        case Source::GameBanana:
            return tr("GameBanana");
        case Source::GitHub:
            return tr("GitHub");
        case Source::Demo:
            return tr("Sample");
        case Source::Submission:
            return tr("Lichii catalog");
        }
        return {};
    }

    QString Byline(const Mod& mod, bool detail = false) const {
        const auto local_date = mod.updated_at.toLocalTime().date();
        const auto now = QDate::currentDate();
        const auto date = local_date == now ? tr("today")
                          : local_date == now.addDays(-1)
                              ? tr("yesterday")
                              : locale().toString(local_date, QLocale::ShortFormat);
        return detail ? tr("by %1 · %2 · updated %3").arg(mod.author, TypeName(mod.type), date)
                      : tr("by %1 · %2 · %3").arg(mod.author, TypeName(mod.type), date);
    }

    const QList<InstalledMod>& InstalledFor(const QString& id) {
        auto found = installed_by_title.constFind(id);
        if (found == installed_by_title.cend()) {
            found = installed_by_title.insert(id, ReadInstalledMods(context.mods_root, id));
        }
        return found.value();
    }

    bool IsEnabled(const QString& target, const QString& id) const {
        return !context.is_enabled || context.is_enabled(target, id);
    }

    void SetCardStatus(ModCard* card, const Mod& mod) {
        bool installed = false;
        for (const auto& target : mod.title_ids) {
            if (!title.isEmpty() && title != target) {
                continue;
            }
            const auto& mods = InstalledFor(target);
            if (std::ranges::find(mods, mod.id, &InstalledMod::id) != mods.cend()) {
                if (IsEnabled(target, mod.id)) {
                    card->SetInstalled(tr("Installed"), true);
                    return;
                }
                installed = true;
            }
        }
        card->SetInstalled(installed ? tr("Off") : QString{}, false);
    }

    void LoadCardThumbnails() {
        while (thumbnail_requests.size() < ThumbnailConcurrency && !thumbnail_queue.isEmpty()) {
            const auto id = thumbnail_queue.takeFirst();
            const auto epoch = list_epoch;
            thumbnail_requests.insert(
                id, client.GetPreview(catalog[id], [this, id, epoch](auto response) {
                    if (epoch != list_epoch) {
                        return;
                    }
                    thumbnail_requests.remove(id);
                    if (const auto* image = std::get_if<QImage>(&response)) {
                        cards[id]->SetImage(
                            image->scaled(600, 338, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                    }
                    LoadCardThumbnails();
                }));
        }
    }

    void FetchMods(int offset, int epoch) {
        more->setEnabled(false);
        if (cards.isEmpty()) {
            browse_message->setText(tr("Loading mods…"));
            browse_message->show();
        } else {
            Status(tr("Loading mods…"));
        }
        list_request =
            client.GetMods(list_filter, offset, ModPageSize, [this, offset, epoch](auto response) {
                if (epoch != list_epoch) {
                    return;
                }
                if (const auto* error = std::get_if<Error>(&response)) {
                    const auto message =
                        tr("Could not load the catalog: %1. Retry with Refresh; installed mods "
                           "are still available.")
                            .arg(error->message);
                    if (cards.isEmpty()) {
                        browse_message->setText(message);
                        browse_message->show();
                    } else {
                        Status(message);
                    }
                    more->setEnabled(true);
                    return;
                }
                auto page = std::get<Page<Mod>>(std::move(response));
                for (auto& mod : page.items) {
                    if (catalog.contains(mod.id)) {
                        continue;
                    }
                    const auto id = mod.id;
                    const auto position = static_cast<int>(cards.size());
                    auto* card =
                        new ModCard(mod.name, Byline(mod), SourceName(mod),
                                    mod.source == Source::GameBanana ? tr("Unverified") : QString{},
                                    browse_page);
                    card->setToolTip(mod.summary);
                    SetCardStatus(card, mod);
                    card_grid->addWidget(card, position / CardColumns, position % CardColumns);
                    cards.insert(id, card);
                    connect(card, &QAbstractButton::clicked, this, [this, id] { SelectMod(id); });
                    if (mod.preview_url.isValid() && !mod.preview_url.isEmpty()) {
                        thumbnail_queue.append(id);
                    }
                    catalog.insert(id, std::move(mod));
                }
                LoadCardThumbnails();
                next = page.next_offset && *page.next_offset > offset ? page.next_offset
                                                                      : std::nullopt;
                more->setVisible(next.has_value() && !debounce.isActive() && !reload_after_install);
                more->setEnabled(true);
                count->setText(catalog.size() == 1
                                   ? tr("1 result")
                                   : tr("%n results", nullptr, static_cast<int>(catalog.size())));
                browse_message->setText(
                    tr("No matching mods. Change the game, type, or search filter."));
                browse_message->setVisible(catalog.isEmpty());
                Status({});
            });
    }

    void FocusInstalledRow(const QString& id, const QString& target) {
        if (auto* button = installed_rows.value(target + u'/' + id)) {
            button->setFocus();
        }
    }

    void ShowInstalled() {
        installed_rows.clear();
        while (auto* item = installed_list->takeAt(0)) {
            item->widget()->hide();
            item->widget()->deleteLater();
            delete item;
        }
        QStringList titles{title};
        if (title.isEmpty()) {
            titles = QDir(context.mods_root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        }
        int displayed = 0;
        const auto query = search->text().trimmed();
        auto* panel = installed_list->parentWidget();
        for (const auto& target : titles) {
            for (const auto& installed : ReadInstalledMods(context.mods_root, target)) {
                if (!query.isEmpty() && !installed.name.contains(query, Qt::CaseInsensitive)) {
                    continue;
                }
                if (displayed++ > 0) {
                    auto* rule = new QFrame(panel);
                    rule->setObjectName(QStringLiteral("storeRule"));
                    rule->setFixedHeight(1);
                    installed_list->addWidget(rule);
                }
                auto* row = new QWidget(panel);
                row->setObjectName(QStringLiteral("storeContents"));
                row->setMinimumHeight(78);
                auto* layout = new QHBoxLayout(row);
                layout->setContentsMargins(0, 12, 6, 12);
                layout->setSpacing(14);
                const auto id = installed.id;
                auto* art = new PreviewImage(10, true, row);
                art->setFixedSize(96, 54);
                if (auto* card = cards.value(id)) {
                    art->SetImage(card->Image());
                }
                layout->addWidget(art);
                auto* info = new QVBoxLayout;
                info->setSpacing(4);
                auto button_text = installed.name;
                button_text.replace(u'&', QStringLiteral("&&"));
                auto* heading = new QPushButton(button_text, row);
                heading->setObjectName(QStringLiteral("storeRowTitle"));
                heading->setAccessibleName(installed.name);
                heading->setFont(Spotlight::Font(15, QFont::DemiBold));
                heading->setAutoDefault(false);
                heading->setCursor(Qt::PointingHandCursor);
                heading->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
                heading->setToolTip(tr("View details for %1").arg(installed.name));
                connect(heading, &QAbstractButton::clicked, this,
                        [this, id, target] { SelectMod(id, target); });
                auto* subtitle = PlainLabel(row, true, Color::TextSecondary(), 12);
                if (const auto entry = catalog.constFind(id); entry != catalog.cend()) {
                    subtitle->setText(tr("by %1 · %2 · Release %3 · %4")
                                          .arg(entry->author, GameName(target), installed.version,
                                               SourceName(entry.value())));
                } else {
                    subtitle->setText(
                        tr("%1 · Release %2").arg(GameName(target), installed.version));
                }
                info->addWidget(heading);
                info->addWidget(subtitle);
                layout->addLayout(info, 1);
                auto* toggle = new ToggleSwitch(row);
                toggle->setAccessibleName(tr("Enable %1").arg(installed.name));
                toggle->setToolTip(tr("Enable or disable %1").arg(installed.name));
                toggle->setEnabled(static_cast<bool>(context.set_enabled));
                toggle->setChecked(IsEnabled(target, id));
                connect(toggle, &QAbstractButton::toggled, this,
                        [this, toggle, target, id](bool on) {
                            if (!SetEnabledFor(target, id, on)) {
                                const QSignalBlocker block(toggle);
                                toggle->setChecked(!on);
                            }
                        });
                layout->addWidget(toggle);
                auto* actions = new QToolButton(row);
                actions->setObjectName(QStringLiteral("storeRowMenu"));
                actions->setAutoRaise(true);
                actions->setFocusPolicy(Qt::StrongFocus);
                actions->setPopupMode(QToolButton::InstantPopup);
                actions->setIcon(Spotlight::Icon(u"dots-three", Color::TextSecondary(), 20));
                actions->setIconSize({20, 20});
                actions->setAccessibleName(tr("Actions for %1").arg(installed.name));
                actions->setToolTip(actions->accessibleName());
                auto* menu = new QMenu(actions);
                menu->addAction(tr("View details"), this,
                                [this, id, target] { SelectMod(id, target); });
                menu->addAction(tr("Uninstall"), this,
                                [this, installed, target] { RemoveLocal(installed, target); });
                actions->setMenu(menu);
                layout->addWidget(actions);
                installed_rows.insert(target + u'/' + id, heading);
                installed_list->addWidget(row);
            }
        }
        installed_count->setText(tr("From the Mod Store · %n", nullptr, displayed));
        if (displayed == 0) {
            auto* empty = PlainLabel(panel, true, Color::TextSecondary(), 14);
            empty->setMinimumHeight(94);
            empty->setAlignment(Qt::AlignCenter);
            empty->setText(tr("No installed store mods match this filter."));
            installed_list->addWidget(empty);
        }
    }

    void ClearDetail() {
        ++mod_epoch;
        if (mod_request) {
            mod_request->Cancel();
        }
        if (preview_request) {
            preview_request->Cancel();
        }
        selected.reset();
        offer_list.clear();
        local.reset();
        local_id.clear();
        local_title.clear();
        name->clear();
        byline->clear();
        source_info_row->hide();
        description->clear();
        description_toggle->hide();
        detail_page->verticalScrollBar()->setValue(0);
        preview->SetImage({});
        gallery->hide();
        preview_count->hide();
        preview_strip->hide();
        preview_buttons.clear();
        preview_images.clear();
        preview_urls.clear();
        current_preview = 0;
        preview_loading = false;
        while (auto* item = thumbnails->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        compatibility_card->hide();
        target_picker->hide();
        release->hide();
        hint->clear();
        hint->hide();
        source->hide();
        install->hide();
        enable->hide();
        uninstall->hide();
    }

    void SelectMod(const QString& id, const QString& target = {}) {
        ClearDetail();
        Status({});
        local_id = id;
        local_title = target;
        pages->setCurrentWidget(detail_page);
        back->setAccessibleName(tr("Back to mod list"));
        back->setToolTip(tr("Back to mod list"));
        back->setFocus();
        if (installed_mode->isChecked()) {
            ReadLocal();
            if (local) {
                name->setText(local->name);
                byline->setText(tr("Installed release: %1").arg(local->version));
                UpdateButtons();
            }
        } else {
            const auto& entry = catalog[id];
            gallery->setVisible(!entry.preview_url.isEmpty());
            name->setText(entry.name);
            byline->setText(Byline(entry, true));
            description->setText(entry.summary);
        }
        FetchDetail(id);
    }

    void FetchDetail(const QString& mod_id) {
        const auto epoch = mod_epoch;
        mod_request = client.GetMod(mod_id, [this, epoch](auto response) {
            if (epoch != mod_epoch) {
                return;
            }
            if (const auto* error = std::get_if<Error>(&response)) {
                hint->setText(tr("Could not load details: %1").arg(error->message));
                hint->show();
                return;
            }
            selected = std::get<Mod>(std::move(response));
            if (local_title.isEmpty()) {
                local_title = title;
                if (local_title.isEmpty()) {
                    const auto found =
                        std::ranges::find_if(selected->title_ids, [this](const QString& id) {
                            return Identity(id).installed;
                        });
                    local_title =
                        found == selected->title_ids.cend() ? selected->title_ids.value(0) : *found;
                }
            }
            {
                const QSignalBlocker block(target_picker);
                target_picker->clear();
                for (const auto& id : selected->title_ids) {
                    target_picker->addItem(GameName(id), id);
                }
                target_picker->setCurrentIndex(target_picker->findData(local_title));
                target_picker->setVisible(title.isEmpty() && !installed_mode->isChecked() &&
                                          target_picker->count() > 1);
            }
            ReadLocal();
            name->setText(selected->name);
            byline->setText(Byline(*selected, true));
            source_info->setText(SourceName(*selected));
            source_info_row->show();
            const auto full = selected->description.trimmed();
            description_excerpt = full.section(QLatin1Char('\n'), 0, 1).simplified();
            if (description_excerpt.size() > 200) {
                description_excerpt.truncate(200);
                if (description_excerpt.back().isHighSurrogate()) {
                    description_excerpt.chop(1);
                }
            }
            const bool truncated = description_excerpt != full.simplified();
            if (truncated) {
                description_excerpt = description_excerpt.trimmed() + QStringLiteral("…");
            }
            const QSignalBlocker description_signals(description_toggle);
            description_toggle->setChecked(false);
            description_toggle->setText(tr("Show full description"));
            description_toggle->setVisible(truncated);
            description->setText(truncated ? description_excerpt : full);
            source->setText(tr("View on %1").arg(SourceName(*selected)));
            source->setVisible(!selected->source_url.isEmpty());
            BuildGallery();
            BuildOffers();
        });
    }

    void BuildGallery() {
        if (selected->external) {
            preview_urls = selected->external->preview_urls;
        }
        if (preview_urls.isEmpty() && selected->preview_url.isValid() &&
            !selected->preview_url.isEmpty()) {
            preview_urls.append(selected->preview_url);
        }
        gallery->setVisible(!preview_urls.isEmpty());
        if (preview_urls.isEmpty()) {
            return;
        }
        const bool show_strip = preview_urls.size() > 1;
        preview_strip->setVisible(show_strip);
        preview_count->setVisible(show_strip);
        if (show_strip) {
            for (int i = 0; i < preview_urls.size(); ++i) {
                auto* button = new PreviewButton(thumbnails->parentWidget());
                button->setAccessibleName(tr("Preview %1").arg(i + 1));
                button->setToolTip(button->accessibleName());
                thumbnails->addWidget(button);
                preview_buttons.append(button);
                connect(button, &QAbstractButton::clicked, this, [this, i] { SelectPreview(i); });
            }
            thumbnails->addStretch();
        }
        SelectPreview(0);
    }

    void SelectPreview(int index) {
        current_preview = index;
        for (int i = 0; i < preview_buttons.size(); ++i) {
            preview_buttons[i]->setChecked(i == index);
        }
        preview_count->setText(tr("Preview %1 of %2").arg(index + 1).arg(preview_urls.size()));
        const auto image = preview_images.constFind(index);
        preview->SetImage(image == preview_images.cend() ? QImage{} : image.value());
        preview->setToolTip(image == preview_images.cend() ? tr("Loading preview…")
                            : image->isNull()              ? tr("Preview unavailable")
                                                           : QString{});
        FetchNextPreview();
    }

    void FetchNextPreview() {
        if (preview_loading) {
            return;
        }
        int index = current_preview;
        if (preview_images.contains(index)) {
            index = 0;
            while (index < preview_urls.size() && preview_images.contains(index)) {
                ++index;
            }
            if (index >= preview_urls.size()) {
                return;
            }
        }
        auto mod = *selected;
        mod.preview_url = preview_urls[index];
        preview_loading = true;
        const auto epoch = mod_epoch;
        preview_request = client.GetPreview(mod, [this, epoch, index](auto response) {
            if (epoch != mod_epoch) {
                return;
            }
            preview_loading = false;
            QImage picture;
            if (const auto* image = std::get_if<QImage>(&response)) {
                picture =
                    image->width() > 1200 || image->height() > 675
                        ? image->scaled(1200, 675, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                        : *image;
            }
            preview_images.insert(index, picture);
            if (!picture.isNull() && index < preview_buttons.size()) {
                preview_buttons[index]->SetImage(
                    picture.scaled(152, 88, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
            if (index == current_preview) {
                SelectPreview(index);
                return;
            }
            FetchNextPreview();
        });
    }

    void BuildOffers() {
        const auto identity =
            selected->title_ids.contains(local_title) ? Identity(local_title) : GameIdentity{};
        const QSignalBlocker block(release);
        offer_list.clear();
        release->clear();
        int first_match = -1;
        if (selected->github) {
            int first_installable = -1;
            int first_unknown = -1;
            for (const auto& build : selected->github->builds) {
                const auto match = MatchGitHubBuild(identity, build);
                QString version;
                if (build.download) {
                    const auto& build_file = *build.download;
                    const QJsonArray selection{QString::fromLatin1(build_file.sha256.toHex()),
                                               static_cast<int>(build_file.patch.kind),
                                               build_file.patch.member, build_file.patch.tag};
                    const auto fingerprint = QCryptographicHash::hash(
                        QJsonDocument(selection).toJson(QJsonDocument::Compact),
                        QCryptographicHash::Sha256);
                    version = build.id.toLower() + u'-' + QString::fromLatin1(fingerprint.toHex());
                }
                offer_list.append({build, version, match, build.download.has_value()});
                const auto display = build.id.size() == 16
                                         ? build.id
                                         : QStringLiteral("%1…%2").arg(build.id.left(12),
                                                                      build.id.right(8));
                release->addItem(tr("%1 · %2").arg(display, CompatibilityName(match)));
                const auto index = release->count() - 1;
                release->setItemData(index, build.id, Qt::ToolTipRole);
                if (first_match < 0 && match == Compatibility::Match && build.download) {
                    first_match = index;
                } else if (first_installable < 0 && CanInstall(offer_list.back())) {
                    first_installable = index;
                }
                if (first_unknown < 0 && match != Compatibility::Mismatch) {
                    first_unknown = index;
                }
            }
            if (first_match < 0) {
                first_match = first_installable < 0 ? first_unknown : first_installable;
            }
        } else if (selected->external) {
            bool first_match_has_checksum = false;
            for (const auto& file : selected->external->files) {
                offer_list.append(
                    {file, {}, Compatibility::Unknown,
                     file.name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive) &&
                         file.size_bytes <= MaxArchiveBytes});
                const auto entry =
                    tr("%1 · %2 · %3")
                        .arg(file.name, locale().formattedDataSize(file.size_bytes),
                             locale().toString(file.uploaded_at.date(), QLocale::ShortFormat));
                release->addItem(entry);
                release->setItemData(release->count() - 1, entry, Qt::ToolTipRole);
                if ((first_match < 0 || (!first_match_has_checksum && !file.sha256.isEmpty())) &&
                    CanInstall(offer_list.back())) {
                    first_match = release->count() - 1;
                    first_match_has_checksum = !file.sha256.isEmpty();
                }
            }
        } else {
            for (const auto& version : selected->versions) {
                const auto match = MatchVersion(local_title, identity, version);
                offer_list.append({version, version.id, match, true});
                release->addItem(
                    tr("%1 · %2 · %3")
                        .arg(version.id, version.game_version, CompatibilityName(match)));
                if (first_match < 0 && match == Compatibility::Match) {
                    first_match = release->count() - 1;
                }
            }
        }
        release->setCurrentIndex(first_match < 0 ? 0 : first_match);
        release->setVisible(release->count() > 0);
        UpdateOffer();
    }

    const Offer* CurrentOffer() const {
        const auto index = release->currentIndex();
        return index >= 0 && index < offer_list.size() ? &offer_list[index] : nullptr;
    }

    bool CanInstall(const Offer& offer) {
        if (!selected || selected->demo || !selected->title_ids.contains(local_title) ||
            !offer.supported || !Identity(local_title).installed ||
            offer.compatibility == Compatibility::Mismatch) {
            return false;
        }
        if (const auto* file = std::get_if<ExternalFile>(&offer.file)) {
            return !file->ambiguous_layout && (file->sha256.isEmpty() || file->sha256.size() == 32);
        }
        if (const auto* github = std::get_if<GitHubBuild>(&offer.file)) {
            return github->download.has_value();
        }
        return offer.compatibility == Compatibility::Match &&
               std::get<Version>(offer.file).sha256.size() == 32;
    }

    void UpdateOffer() {
        if (!selected) {
            return;
        }
        const auto identity =
            selected->title_ids.contains(local_title) ? Identity(local_title) : GameIdentity{};
        const auto* offer = CurrentOffer();
        const auto unknown = tr("Unknown");
        const auto short_build = [](const QString& build) {
            return build.size() > 16 ? QStringLiteral("%1…%2").arg(build.left(12), build.right(8))
                                     : build;
        };
        const auto set_row = [&](size_t row, const QString& key, const QString& value,
                                 const QString& tooltip = {}) {
            const auto [left, right] = compatibility_rows[row];
            left->setText(key);
            right->setText(value);
            right->setToolTip(tooltip);
            left->setVisible(!key.isEmpty());
            right->setVisible(!key.isEmpty());
        };
        set_row(0, tr("Game"), local_title.isEmpty() ? unknown : GameName(local_title));
        set_row(1, tr("Title ID"), local_title);
        set_row(2, tr("Target game version"), unknown);
        set_row(3, tr("Target build ID"), unknown);
        set_row(4, tr("Minimum Lichii"), unknown);
        set_row(5, tr("Installed version"),
                identity.display_version.isEmpty()
                    ? (identity.installed ? unknown : tr("Not in your library"))
                    : identity.display_version);
        set_row(6, tr("Installed build ID"),
                KnownBuild(identity.build_id) ? short_build(identity.build_id) : unknown,
                KnownBuild(identity.build_id) ? identity.build_id : QString{});
        set_row(7, tr("Running Lichii"),
                Common::g_build_release_tag[0] ? QString::fromLatin1(Common::g_build_release_tag)
                                               : tr("Release number unavailable"));
        const auto* external = offer ? std::get_if<ExternalFile>(&offer->file) : nullptr;
        catalog_note->setVisible(external && external->sha256.isEmpty());
        const auto match = offer ? offer->compatibility : Compatibility::Unknown;
        verification->setText(CompatibilityName(match));
        verification->setVisible(offer || selected->github.has_value());
        const auto color = match == Compatibility::Match      ? Color::Success()
                           : match == Compatibility::Mismatch ? Color::Accent()
                                                              : Color::Warning();
        const auto state_style =
            QStringLiteral("color: %1; background: transparent;").arg(CssColor(color));
        verification->setStyleSheet(state_style);
        compatibility_state->setStyleSheet(state_style);
        compatibility_state->setText(CompatibilityName(match));
        set_row(8, selected->external ? tr("Source") : QString{},
                selected->external
                    ? tr("%1 · %2").arg(SourceName(*selected), selected->external->category)
                    : QString{});
        set_row(9, external ? tr("File") : QString{},
                external ? tr("%1 · %2").arg(external->name,
                                             locale().formattedDataSize(external->size_bytes))
                         : QString{});
        set_row(10, external ? tr("Uploaded") : QString{},
                external ? locale().toString(external->uploaded_at.toLocalTime().date(),
                                             QLocale::ShortFormat)
                         : QString{});
        github_provenance->setVisible(selected->github.has_value());
        if (selected->github) {
            const auto& github = *selected->github;
            set_row(2, {}, {});
            set_row(4, {}, {});
            if (const auto* selected_build = offer ? std::get_if<GitHubBuild>(&offer->file) : nullptr) {
                const auto& build = *selected_build;
                set_row(3, build.main && build.id.size() == 16 ? tr("Main build ID prefix")
                           : build.main                       ? tr("Main build ID")
                                                              : tr("Patch build ID"),
                        short_build(build.id), build.id);
            }
            github_provenance->setText(
                tr("Source: %1 @ %2\nLicense: Not declared")
                    .arg(github.repository, github.commit.left(12)));
            github_provenance->setToolTip(github.commit);
        }
        if (offer) {
            if (const auto* version = std::get_if<Version>(&offer->file)) {
                set_row(1, tr("Title ID"), version->title_id);
                set_row(2, tr("Target game version"), version->game_version);
                set_row(3, tr("Target build ID"), short_build(version->build_id),
                        version->build_id);
                set_row(4, tr("Minimum Lichii"), version->min_lichii_build);
            } else if (const auto* file = std::get_if<ExternalFile>(&offer->file)) {
                QString visible_builds = unknown;
                if (!file->build_ids.isEmpty()) {
                    visible_builds = short_build(file->build_ids.front());
                    if (file->build_ids.size() > 1) {
                        visible_builds = tr("%1 and %n more", nullptr,
                                            static_cast<int>(file->build_ids.size() - 1))
                                             .arg(visible_builds);
                    }
                }
                set_row(3, tr("Patch build IDs"), visible_builds,
                        file->build_ids.join(QLatin1Char('\n')));
            }
        }
        set_row(11, tr("Installed"), {});
        compatibility_state->setVisible(offer != nullptr || selected->github.has_value());
        compatibility_card->show();
        UpdateButtons();
    }

    void ReadLocal() {
        local.reset();
        if (local_title.isEmpty() || local_id.isEmpty()) {
            return;
        }
        const auto installed = ReadInstalledMods(context.mods_root, local_title);
        const auto it = std::ranges::find(installed, local_id, &InstalledMod::id);
        if (it != installed.cend()) {
            local = *it;
        }
    }

    void UpdateButtons() {
        const auto* offer = CurrentOffer();
        const auto* external = offer ? std::get_if<ExternalFile>(&offer->file) : nullptr;
        const bool installed = local.has_value();
        const bool same_file =
            external && installed && local->version.startsWith(ExternalVersionPrefix(external->id));
        const bool reinstall = same_file && external->sha256.isEmpty();
        const bool current =
            offer && installed &&
            (external ? same_file && !reinstall &&
                            (local->sha256.isEmpty()
                                 ? local->version == ExternalVersion(external->id, external->sha256)
                                 : local->sha256 == external->sha256)
                      : local->version == offer->version);
        install->setVisible(offer && (!selected->github || offer->supported));
        install->setText(reinstall ? tr("Reinstall") : installed ? tr("Update") : tr("Install"));
        install->setEnabled(!busy && offer && !current && CanInstall(*offer));
        source->setEnabled(!busy);
        const bool install_primary = install->isEnabled();
        const bool source_primary = !install_primary && !source->isHidden() && !busy;
        install->SetKind(install_primary ? PillButton::Kind::Accent : PillButton::Kind::Control);
        source->SetKind(source_primary ? PillButton::Kind::Accent : PillButton::Kind::Control);
        if (mod_actions->indexOf(source_primary ? source : install) != 0) {
            auto* first = source_primary ? source : install;
            mod_actions->removeWidget(first);
            mod_actions->insertWidget(0, first);
        }
        enable->setVisible(installed);
        uninstall->setVisible(installed);
        enable->setEnabled(!busy && static_cast<bool>(context.set_enabled));
        uninstall->setEnabled(!busy);
        const bool enabled = installed && IsEnabled(local_title, local_id);
        if (selected) {
            compatibility_rows[11].second->setText(installed ? (enabled ? tr("Enabled") : tr("Off"))
                                                             : tr("Not installed"));
        }
        enable->setText(enabled ? tr("Disable") : tr("Enable"));
        hint->clear();
        if (selected && selected->github && (!offer || !offer->supported)) {
            hint->setText(tr("No verified download is available for this patch. "
                             "Open GitHub for the files and instructions."));
        } else if (offer && selected) {
            if (selected->demo) {
                hint->setText(tr("This sample is not available for installation."));
            } else if (external && external->ambiguous_layout) {
                hint->setText(tr("This ZIP requires choosing components. Open GameBanana to "
                                 "install it manually."));
            } else if (!selected->title_ids.contains(local_title)) {
                hint->setText(tr("This listing does not target the selected game."));
            } else if (offer->compatibility == Compatibility::Mismatch) {
                hint->setText(
                    tr("This release does not match the installed game or Lichii build."));
            } else if (!offer->supported) {
                hint->setText(tr("Only ZIP files up to 64 MiB can be installed. Open GameBanana "
                                 "for this file."));
            } else if (!Identity(local_title).installed) {
                hint->setText(tr("Add this game to your library to install its mods."));
            } else if (!CanInstall(*offer)) {
                hint->setText(tr("The installed game or Lichii build could not be verified."));
            } else if (current || reinstall) {
                hint->setText(enabled ? tr("Installed and enabled") : tr("Installed and disabled"));
            } else {
                hint->setText(installed ? tr("A different release is available.") : QString{});
            }
        } else if (installed) {
            hint->setText(enabled ? tr("Installed and enabled") : tr("Installed and disabled"));
        }
        hint->setVisible(!hint->text().isEmpty());
    }

    void OpenSource() {
        if (selected && !selected->source_url.isEmpty()) {
            QDesktopServices::openUrl(selected->source_url);
        }
    }

    void InstallFailed(const QString& error) {
        Status(error);
        if (!selected || selected->source_url.isEmpty()) {
            return;
        }
        const QUrl source_url = selected->source_url;
        QMessageBox prompt(QMessageBox::Warning, tr("Could not install the mod"), error,
                           QMessageBox::Close, this);
        auto* open = prompt.addButton(source->text(), QMessageBox::ActionRole);
        prompt.exec();
        if (prompt.clickedButton() == open && !source_url.isEmpty()) {
            QDesktopServices::openUrl(source_url);
        }
    }

    void SetProgress(const QString& text, int percent = -1, bool can_cancel = false) {
        if (text.isEmpty()) {
            progress->hide();
            return;
        }
        progress_label->setText(text);
        cancel_button->setVisible(can_cancel);
        progress_bar->setRange(0, percent < 0 ? 0 : 100);
        if (percent >= 0) {
            progress_bar->setValue(percent);
        }
        progress->show();
    }

    void SetBusy(bool value) {
        busy = value;
        for (QWidget* widget :
             {static_cast<QWidget*>(game_picker), static_cast<QWidget*>(refresh),
              static_cast<QWidget*>(installed_mode), static_cast<QWidget*>(search),
              static_cast<QWidget*>(preview_strip), static_cast<QWidget*>(browse_page),
              static_cast<QWidget*>(installed_page), static_cast<QWidget*>(release),
              static_cast<QWidget*>(target_picker), static_cast<QWidget*>(back)}) {
            widget->setEnabled(!value);
        }
        UpdateButtons();
    }

    void StartInstall() {
        const auto* offer = CurrentOffer();
        if (!offer || !CanInstall(*offer) || busy) {
            return;
        }
        const auto chosen = *offer;
        const auto* external_file = std::get_if<ExternalFile>(&chosen.file);
        const auto* github = std::get_if<GitHubBuild>(&chosen.file);
        const auto patch = github ? std::optional{github->download->patch} : std::nullopt;
        const bool external = external_file != nullptr;
        const bool unverified = external && external_file->sha256.isEmpty();
        const auto label = selected->name;
        const auto epoch = mod_epoch;
        if ((external || patch) && !unverified && chosen.compatibility == Compatibility::Unknown &&
            QMessageBox::question(
                this, tr("Compatibility unverified"),
                tr("Lichii could not confirm that this file works with your game version. "
                   "It will check the download's SHA-256 before installing. Continue?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        if (epoch != mod_epoch) {
            return;
        }
        reload_after_install = debounce.isActive();
        debounce.stop();
        SetBusy(true);
        progress_catalog_note->setVisible(unverified);
        SetProgress(tr("Downloading %1…").arg(label), 0, true);
        const auto generation = ++install_epoch;
        const auto version = chosen.version;
        const auto file_id = external ? external_file->id : 0;
        auto receive = [this, generation, version, file_id, label, external, patch](auto response) {
            if (generation != install_epoch) {
                return;
            }
            if (const auto* error = std::get_if<Error>(&response)) {
                FinishOperation();
                InstallFailed(tr("Could not download the mod: %1").arg(error->message));
                return;
            }
            auto received = std::get<DownloadedFile>(std::move(response));
            auto owned = std::shared_ptr<QTemporaryFile>(std::move(received.file));
            ApplyDownloaded(owned, external ? ExternalVersion(file_id, received.sha256) : version,
                            received.sha256, label, external, patch);
        };
        auto report = [this, generation, unverified](qint64 received, qint64 total) {
            if (generation != install_epoch || total <= 0) {
                return;
            }
            if (received >= total) {
                SetProgress(unverified ? tr("Checking the download…") : tr("Verifying SHA-256…"));
            } else {
                SetProgress(tr("Downloading %1 of %2 KiB…").arg(received / 1024).arg(total / 1024),
                            static_cast<int>(100 * received / total), true);
            }
        };
        download = std::visit(
            [&](const auto& file) {
                if constexpr (std::is_same_v<std::decay_t<decltype(file)>, GitHubBuild>) {
                    return client.Download(*file.download, std::move(receive), std::move(report));
                } else {
                    return client.Download(file, std::move(receive), std::move(report));
                }
            },
            chosen.file);
    }

    void ApplyDownloaded(const std::shared_ptr<QTemporaryFile>& file, const QString& version,
                         const QByteArray& digest, const QString& label, bool external,
                         const std::optional<GitHubPatch>& patch) {
        SetProgress(patch ? tr("Checking and installing the patch…")
                          : tr("Checking the ZIP and installing the mod…"));
        applying = true;
        const auto root = context.mods_root;
        const auto target = local_title;
        const auto id = local_id;
        auto* install_task = new QFutureWatcher<QString>(this);
        connect(install_task, &QFutureWatcher<QString>::finished, this, [this, install_task] {
            const auto error = install_task->result();
            install_task->deleteLater();
            if (error.isEmpty()) {
                changed = true;
                InstalledChanged(local_title, local_id);
                ReadLocal();
            }
            FinishOperation();
            if (error.isEmpty()) {
                Status(tr("Installation complete. The mod is available in Add-Ons."));
            } else {
                InstallFailed(error);
            }
        });
        install_task->setFuture(QtConcurrent::run([root, target, id, label, version, file, digest,
                                                   external, patch] {
            if (patch) {
                return InstallGitHubPatch(root, target, id, label, version, file->fileName(), digest,
                                          *patch);
            }
            return InstallMod(root, target, id, label, version, file->fileName(), digest, external);
        }));
    }

    void FinishOperation() {
        applying = false;
        SetProgress({});
        SetBusy(false);
        if (reload_after_install) {
            reload_after_install = false;
            debounce.start();
        }
    }

    void CancelDownload() {
        if (applying || !busy) {
            return;
        }
        ++install_epoch;
        if (download) {
            download->Cancel();
        }
        FinishOperation();
        Status(tr("Download canceled. No add-ons were changed."));
    }

    void ToggleEnabled() {
        if (!local || !context.set_enabled || busy) {
            return;
        }
        if (SetEnabledFor(local_title, local_id, !IsEnabled(local_title, local_id))) {
            UpdateButtons();
        }
    }

    bool SetEnabledFor(const QString& target, const QString& id, bool enabled) {
        if (!context.set_enabled || busy || !context.set_enabled(target, id, enabled)) {
            Status(tr("Could not save the change to this mod."));
            return false;
        }
        changed = true;
        InstalledChanged(target, id);
        Status(enabled ? tr("Mod enabled.") : tr("Mod disabled."));
        return true;
    }

    void InstalledChanged(const QString& target, const QString& id) {
        installed_by_title.remove(target);
        if (auto* card = cards.value(id)) {
            SetCardStatus(card, catalog[id]);
        }
    }

    void RemoveInstalled() {
        if (local) {
            RemoveLocal(*local, local_title);
        }
    }

    void RemoveLocal(InstalledMod installed, QString target) {
        if (busy) {
            return;
        }
        const auto epoch = mod_epoch;
        if (QMessageBox::question(
                this, tr("Uninstall mod"), tr("Uninstall %1?").arg(installed.name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes ||
            epoch != mod_epoch) {
            return;
        }
        const auto error = UninstallMod(context.mods_root, target, installed.id);
        if (!error.isEmpty()) {
            Status(error);
            return;
        }
        const bool preference_reset =
            !context.set_enabled || context.set_enabled(target, installed.id, true);
        changed = true;
        local.reset();
        InstalledChanged(target, installed.id);
        if (installed_mode->isChecked()) {
            ClearDetail();
            RestoreListPage();
            ShowInstalled();
        } else {
            UpdateButtons();
        }
        Status(preference_reset
                   ? tr("Mod uninstalled.")
                   : tr("Mod uninstalled, but Lichii could not clear its saved disabled state."));
    }

    DialogContext context;
    Client client;
    QTimer debounce;
    QList<Game> games;
    QList<Game> incoming_games;
    QHash<QString, Mod> catalog;
    QHash<QString, ModCard*> cards;
    QHash<QString, QPushButton*> installed_rows;
    QHash<QString, QList<InstalledMod>> installed_by_title;
    QList<QString> thumbnail_queue;
    QHash<QString, QPointer<Request>> thumbnail_requests;
    QHash<QString, GameIdentity> identities;
    QList<Offer> offer_list;
    ModFilter list_filter{};
    std::optional<int> next;
    std::optional<Mod> selected;
    std::optional<InstalledMod> local;
    QString description_excerpt;
    QString title;
    QString local_title;
    QString local_id;
    QPointer<Request> games_request;
    QPointer<Request> list_request;
    QPointer<Request> mod_request;
    QPointer<Request> preview_request;
    QPointer<Request> download;
    QList<QUrl> preview_urls;
    QList<PreviewButton*> preview_buttons;
    QHash<int, QImage> preview_images;
    int current_preview{};
    bool preview_loading{};
    int game_epoch{};
    int list_epoch{};
    int mod_epoch{};
    int install_epoch{};
    bool changed{};
    bool busy{};
    bool applying{};
    bool reload_after_install{};
    QStackedWidget* pages{};
    QWidget* browse_page{};
    QScrollArea* browse_scroll{};
    QScrollArea* installed_page{};
    QScrollArea* detail_page{};
    QScrollArea* preview_strip{};
    QWidget* gallery{};
    QGridLayout* card_grid{};
    QVBoxLayout* installed_list{};
    QHBoxLayout* thumbnails{};
    QLabel* count{};
    QLabel* installed_count{};
    QLabel* browse_message{};
    QComboBox* game_picker{};
    SegmentedControl* type{};
    QComboBox* sort{};
    QComboBox* release{};
    QComboBox* target_picker{};
    QLineEdit* search{};
    QLabel* name{};
    QLabel* byline{};
    QLabel* source_info{};
    QWidget* source_info_row{};
    QLabel* verification{};
    QLabel* description{};
    QToolButton* description_toggle{};
    PreviewImage* preview{};
    QLabel* preview_count{};
    QLabel* compatibility_state{};
    QLabel* catalog_note{};
    QLabel* github_provenance{};
    QLabel* hint{};
    QLabel* status{};
    QFrame* compatibility_card{};
    std::array<std::pair<QLabel*, QLabel*>, 12> compatibility_rows{};
    QHBoxLayout* mod_actions{};
    QDialog* progress{};
    QLabel* progress_label{};
    QLabel* progress_catalog_note{};
    QProgressBar* progress_bar{};
    PillButton* cancel_button{};
    CircleButton* refresh{};
    PillButton* installed_mode{};
    PillButton* more{};
    PillButton* install{};
    PillButton* source{};
    PillButton* enable{};
    PillButton* uninstall{};
    CircleButton* back{};
};

} // namespace

bool ShowDialog(QWidget* parent, DialogContext context) {
    Dialog dialog(parent, std::move(context));
    dialog.exec();
    return dialog.Changed();
}

} // namespace ModStore
