// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <map>
#include <utility>
#include <vector>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFocusEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QVBoxLayout>
#include "common/settings.h"
#include "lichii/configuration/configuration_shared.h"
#include "lichii/configuration/shared_widget.h"
#include "lichii/spotlight/settings_page.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/top_bar.h"
#include "lichii/spotlight/widgets.h"
#include "qt_common/util/vk.h"

namespace Spotlight {
namespace {

QString CssColor(const QColor& color) {
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

constexpr int GroupRadius = 20;

class Surface final : public QWidget {
public:
    Surface(QColor fill_, int radius_, QColor border_ = Qt::transparent)
        : fill{fill_}, border{border_}, radius{radius_} {
        setProperty("spotlightTransparent", true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(fill);
        painter.setPen(border.alpha() == 0 ? QPen(Qt::NoPen) : QPen(border));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }

private:
    QColor fill;
    QColor border;
    int radius;
};

class SettingsBackdrop final : public QWidget {
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QLinearGradient fill(0, 0, width() * 0.3, height());
        fill.setColorAt(0, SettingsColor::BackdropTop());
        fill.setColorAt(1, SettingsColor::BackdropBottom());
        painter.fillRect(rect(), fill);
    }
};

class IconTile final : public QWidget {
public:
    IconTile(QString icon_, QColor color_) : icon{std::move(icon_)}, color{color_} {
        setFixedSize(40, 40);
        setProperty("spotlightTransparent", true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Color::Fill());
        painter.drawRoundedRect(rect(), Radius::Row, Radius::Row);
        painter.drawPixmap(10, 10, IconPixmap(icon, 20, color, devicePixelRatioF()));
    }

private:
    QString icon;
    QColor color;
};

bool KeyboardFocusWithin(const QWidget* widget) {
    const auto* focused = QApplication::focusWidget();
    return focused != nullptr && widget->isAncestorOf(focused) &&
           widget->window()->testAttribute(Qt::WA_KeyboardFocusChange);
}

class SettingsRow final : public QWidget {
public:
    explicit SettingsRow(bool divider_) : divider{divider_} {
        setAttribute(Qt::WA_Hover);
        connect(qApp, &QApplication::focusChanged, this, [this](QWidget* old, QWidget* current) {
            if ((old && isAncestorOf(old)) || (current && isAncestorOf(current))) {
                update();
            }
        });
    }

protected:
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (underMouse() || KeyboardFocusWithin(this)) {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(Color::Accent(), 2));
            painter.setBrush(Color::AccentSoft());
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), Radius::Card,
                                    Radius::Card);
        } else if (divider && !PreviousRowHighlighted()) {
            painter.fillRect(QRect(18, 0, width() - 36, 1), QColor(0xff, 0xff, 0xff, 0x12));
        }
    }

private:
    bool PreviousRowHighlighted() const {
        const auto* layout = parentWidget()->layout();
        const int index = layout->indexOf(this);
        for (int i = index - 1; i >= 0; --i) {
            auto* previous = layout->itemAt(i)->widget();
            if (previous == nullptr || previous->isHidden()) {
                continue;
            }
            return previous->underMouse() || KeyboardFocusWithin(previous);
        }
        return false;
    }

    bool divider;
};

class DescriptionLabel final : public QLabel {
public:
    int heightForWidth(int width) const override {
        return QLabel::heightForWidth(std::min(width, maximumWidth()));
    }
};

class NavigationButton final : public QAbstractButton {
public:
    NavigationButton(const QString& title, QString icon_, QColor icon_color_)
        : icon{std::move(icon_)}, icon_color{icon_color_} {
        setText(title);
        setCheckable(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFixedHeight(44);
    }

protected:
    void focusInEvent(QFocusEvent* event) override {
        keyboard_focus = event->reason() != Qt::MouseFocusReason;
        QAbstractButton::focusInEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        keyboard_focus = false;
        QAbstractButton::mousePressEvent(event);
        update();
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool outlined = hasFocus() && keyboard_focus;
        painter.setPen(outlined ? QPen(Color::Accent()) : QPen(Qt::NoPen));
        if (isChecked() || underMouse() || outlined) {
            painter.setBrush(isChecked()    ? Color::AccentSoft()
                             : underMouse() ? Color::Fill()
                                            : Qt::transparent);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Radius::Row,
                                    Radius::Row);
        }
        painter.drawPixmap(12, (height() - 24) / 2,
                           IconPixmap(icon, 24, icon_color, devicePixelRatioF()));
        painter.setFont(Font(14, isChecked() ? 600 : 500));
        painter.setPen(Color::Text());
        painter.drawText(rect().adjusted(48, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(text(), Qt::ElideRight, width() - 56));
    }

private:
    QString icon;
    QColor icon_color;
    bool keyboard_focus = false;
};

class ManageButton final : public QAbstractButton {
public:
    explicit ManageButton(const QString& label) {
        setText(label);
        setFont(Font(13, 600));
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setProperty("spotlightTransparent", true);
        setFixedSize(fontMetrics().horizontalAdvance(label) + (label.isEmpty() ? 32 : 46), 36);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (hasFocus()) {
            painter.setPen(Color::Accent());
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        }
        const auto ink = !isEnabled()   ? Color::TextFaint()
                         : underMouse() ? Color::Text()
                                        : Color::TextSecondary();
        painter.setPen(ink);
        painter.drawText(rect().adjusted(8, 0, -30, 0), Qt::AlignRight | Qt::AlignVCenter, text());
        painter.drawPixmap(width() - 24, (height() - 16) / 2,
                           IconPixmap(QStringLiteral("caret-right"), 16, ink, devicePixelRatioF()));
    }
};

class ComboPill final : public QComboBox {
public:
    explicit ComboPill(std::function<QString(const QComboBox&, int)> label_ = {})
        : label{std::move(label_)} {
        setFont(Font(13, 600));
        setFixedHeight(34);
        setCursor(Qt::PointingHandCursor);
        setProperty("spotlightTransparent", true);
        connect(this, &QComboBox::currentIndexChanged, this, [this] { Refresh(); });
    }

    void Refresh() {
        const auto display = ClosedText();
        const QFontMetrics metrics(Font(13, 600));
        setFixedWidth(qBound(64, metrics.horizontalAdvance(display) + 50, 260));
        setToolTip(currentText().isEmpty() ? display : currentText());
        update();
    }

    void showPopup() override {
        const QFontMetrics popup_font(view()->font());
        int popup_width = width();
        for (int i = 0; i < count(); ++i) {
            popup_width = qMax(popup_width, popup_font.horizontalAdvance(itemText(i)) + 48);
        }
        view()->setMinimumWidth(popup_width);
        QComboBox::showPopup();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(hasFocus() ? QPen(Color::Accent()) : QPen(Qt::NoPen));
        painter.setBrush(underMouse() && isEnabled() ? Color::Control() : Color::Fill());
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
        const auto ink = isEnabled() ? Color::Text() : Color::TextFaint();
        const QRect label_rect = rect().adjusted(14, 0, -34, 0);
        painter.setFont(Font(13, 600));
        painter.setPen(ink);
        painter.drawText(
            label_rect, Qt::AlignLeft | Qt::AlignVCenter,
            painter.fontMetrics().elidedText(ClosedText(), Qt::ElideRight, label_rect.width()));
        painter.drawPixmap(width() - 26, (height() - 16) / 2,
                           IconPixmap(QStringLiteral("caret-down"), 16,
                                      isEnabled() ? Color::TextSecondary() : Color::TextFaint(),
                                      devicePixelRatioF()));
    }

private:
    QString ClosedText() const {
        if (currentIndex() < 0) {
            return placeholderText();
        }
        const auto short_text = label ? label(*this, currentIndex()) : QString{};
        return short_text.isEmpty() ? currentText() : short_text;
    }

    std::function<QString(const QComboBox&, int)> label;
};

class FieldPill final : public QWidget {
public:
    explicit FieldPill(QWidget* focus_target_) : focus_target{focus_target_} {
        setProperty("spotlightTransparent", true);
        setFixedHeight(34);
        setAttribute(Qt::WA_Hover);
        focus_target->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut ||
            event->type() == QEvent::EnabledChange) {
            update();
        }
        return false;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(focus_target->hasFocus() ? QPen(Color::Accent()) : QPen(Qt::NoPen));
        painter.setBrush(underMouse() && isEnabled() ? Color::Control() : Color::Fill());
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    }

private:
    QWidget* focus_target;
};

QString FieldStyle(const QString& type) {
    return QStringLiteral("%1 { background: transparent; border: none; color: %2; "
                          "selection-background-color: %3; selection-color: %4; }"
                          "%1:disabled { color: %5; }")
        .arg(type, CssColor(Color::Text()), CssColor(Color::Accent()),
             CssColor(Color::OnAccent()), CssColor(Color::TextFaint()));
}

QWidget* NumberField(QAbstractSpinBox* editor) {
    auto* field = new FieldPill(editor);
    auto* layout = new QHBoxLayout(field);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);
    auto* minus = new CircleButton(QStringLiteral("minus"), 26);
    auto* plus = new CircleButton(QStringLiteral("plus"), 26);
    for (auto* step : {minus, plus}) {
        step->setFocusPolicy(Qt::NoFocus);
        step->setAutoRepeat(true);
    }
    minus->setAccessibleName(SettingsPage::tr("Decrease"));
    plus->setAccessibleName(SettingsPage::tr("Increase"));
    editor->setButtonSymbols(QAbstractSpinBox::NoButtons);
    editor->setAlignment(Qt::AlignCenter);
    editor->setFrame(false);
    editor->setFont(Font(13, 600));
    editor->setStyleSheet(FieldStyle(QString::fromLatin1(editor->metaObject()->className())));
    editor->setMinimumWidth(56);
    QObject::connect(minus, &QAbstractButton::clicked, editor, &QAbstractSpinBox::stepDown);
    QObject::connect(plus, &QAbstractButton::clicked, editor, &QAbstractSpinBox::stepUp);
    layout->addWidget(minus);
    layout->addWidget(editor, 1);
    layout->addWidget(plus);
    const auto sync_width = [field, editor] {
        const int text = editor->fontMetrics().horizontalAdvance(editor->text());
        const int extra = qobject_cast<QDateTimeEdit*>(editor) != nullptr ? 124 : 96;
        field->setFixedWidth(qBound(132, text + extra, 300));
    };
    sync_width();
    if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
        QObject::connect(spin, &QSpinBox::textChanged, field, sync_width);
    } else if (auto* real = qobject_cast<QDoubleSpinBox*>(editor)) {
        QObject::connect(real, &QDoubleSpinBox::textChanged, field, sync_width);
    }
    return field;
}

class SlimSlider final : public QSlider {
public:
    SlimSlider() : QSlider(Qt::Horizontal) {
        setFixedSize(196, 20);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setProperty("spotlightTransparent", true);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        setSliderDown(true);
        MoveTo(event->position().x());
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (isSliderDown()) {
            MoveTo(event->position().x());
        }
    }
    void mouseReleaseEvent(QMouseEvent*) override {
        setSliderDown(false);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal span = maximum() - minimum();
        qreal ratio = span > 0 ? (value() - minimum()) / span : 0;
        if (invertedAppearance()) {
            ratio = 1 - ratio;
        }
        const QRectF track(8, height() / 2.0 - 3, width() - 16, 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Color::Stroke());
        painter.drawRoundedRect(track, 3, 3);
        const QColor fill = isEnabled() ? Color::Accent() : Color::TextFaint();
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * ratio, 6), 3, 3);
        const QPointF knob(track.left() + track.width() * ratio, height() / 2.0);
        painter.setBrush(QColor(0, 0, 0, 0x4d));
        painter.drawEllipse(knob + QPointF(0, 1), 8.5, 8.5);
        painter.setBrush(Color::Text());
        painter.setPen(hasFocus() ? QPen(Color::Accent(), 2) : QPen(Qt::NoPen));
        painter.drawEllipse(knob, 8, 8);
    }

private:
    void MoveTo(qreal x) {
        qreal ratio = qBound(0.0, (x - 8) / (width() - 16), 1.0);
        if (invertedAppearance()) {
            ratio = 1 - ratio;
        }
        setSliderPosition(minimum() + qRound(ratio * (maximum() - minimum())));
    }
};

class WatchWidget final : public QObject {
public:
    WatchWidget(QWidget* source, QWidget* target, QEvent::Type event_, std::function<void()> sync_)
        : QObject(target), event{event_}, sync{std::move(sync_)} {
        source->installEventFilter(this);
        sync();
    }

protected:
    bool eventFilter(QObject*, QEvent* change) override {
        if (change->type() == event) {
            sync();
        }
        return false;
    }

private:
    QEvent::Type event;
    std::function<void()> sync;
};

void MirrorEnabled(QWidget* source, QWidget* target) {
    new WatchWidget(source, target, QEvent::EnabledChange,
                    [source, target] { target->setEnabled(source->isEnabled()); });
}

// Follows whether any source control would be shown on its page, through every ancestor up to
// the page, and stores it on the row for SettingsPage to combine with the search.
QLayout* DirectLayout(QLayout* layout, const QWidget* widget) {
    if (layout == nullptr) {
        return nullptr;
    }
    for (int i = 0; i < layout->count(); ++i) {
        QLayoutItem* item = layout->itemAt(i);
        if (item->widget() == widget) {
            return layout;
        }
        if (auto* found = DirectLayout(item->layout(), widget)) {
            return found;
        }
    }
    return nullptr;
}

bool OnSameLine(const QWidget* first, const QWidget* second) {
    if (first->parentWidget() != second->parentWidget() || first->parentWidget() == nullptr) {
        return false;
    }
    QLayout* const layout = DirectLayout(first->parentWidget()->layout(), first);
    if (layout == nullptr || layout != DirectLayout(first->parentWidget()->layout(), second)) {
        return false;
    }
    if (auto* box = qobject_cast<QBoxLayout*>(layout)) {
        return box->direction() == QBoxLayout::LeftToRight ||
               box->direction() == QBoxLayout::RightToLeft;
    }
    if (auto* grid = qobject_cast<QGridLayout*>(layout)) {
        int first_row, second_row, column, span_rows, span_columns;
        grid->getItemPosition(grid->indexOf(first), &first_row, &column, &span_rows, &span_columns);
        grid->getItemPosition(grid->indexOf(second), &second_row, &column, &span_rows,
                              &span_columns);
        return first_row == second_row;
    }
    return false;
}

QString RowIcon(const QString& title, const QString& section, const QString& fallback) {
    struct Rule {
        QRegularExpression pattern;
        QString icon;
    };
    const auto rules = [](std::initializer_list<std::pair<const char*, const char*>> entries) {
        std::vector<Rule> out;
        for (const auto& [words, icon] : entries) {
            const QString pattern = QStringLiteral("\\b(%1)\\b").arg(QLatin1String(words));
            out.push_back({QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption),
                           QLatin1String(icon)});
        }
        return out;
    };
    static const std::vector<Rule> by_title = rules({
        {"log|logs|logging", "file"},
        {"dump|dumps", "export"},
        {"directory|directories|path|root|location", "folder"},
        {"nand|sd card|save data|storage", "hard-drives"},
        {"shader|shaders|spir-v|macro|pipeline", "cube"},
        {"vulkan|gpu|api|device", "graphics-card"},
        {"cpu|cores?|multicore|ticks", "cpu"},
        {"memory|ram", "memory"},
        {"speed|fps|frame rate|limit", "timer"},
        {"clock|clocks|time|date|rtc|time zone", "clock"},
        {"language|translation", "translate"},
        {"region|web|online|host|hosts|server", "globe"},
        {"keyboard", "keyboard"},
        {"mouse|touch|touchscreen|motion|panning", "hand-pointing"},
        {"amiibo", "tag"},
        {"camera|screenshot|screenshots", "camera"},
        {"mute|muted", "speaker-x"},
        {"audio|sound|volume|output", "speaker-high"},
        {"network|airplane|lan|interface", "wifi-high"},
        {"token|key|keys|certificate", "key"},
        {"user|username|profile|nickname", "user"},
        {"fullscreen", "arrows-out"},
        {"resolution|vsync|hdr|display|monitor|aspect", "monitor"},
        {"filter|anti-aliasing|sharpness|anisotropic", "sliders-horizontal"},
        {"applet|applets", "squares-four"},
        {"debug|debugging|debugger|stub|assert|asserts|gdb", "bug"},
        {"controller|controllers|joycon|pro controller|gamepad", "game-controller"},
        {"vibration|rumble", "vibrate"},
        {"cache|caching|metadata", "stack"},
        {"homebrew|arguments", "cube"},
    });
    static const std::vector<Rule> by_section = rules({
        {"debug|debugger|debugging", "bug"},
        {"logging", "file"},
        {"graphics|gpu|api settings|renderer", "graphics-card"},
        {"cpu|core", "cpu"},
        {"audio|output", "speaker-high"},
        {"network|nextendo|web|host redirect", "globe"},
        {"storage directories|filesystem|patch manager|gamecard", "hard-drives"},
        {"caching", "stack"},
        {"applet mode preference|applets", "squares-four"},
        {"homebrew", "cube"},
        {"advanced", "wrench"},
        {"emulated devices|input devices", "game-controller"},
        {"console|system", "desktop"},
        {"ui|interface|screenshots", "palette"},
    });
    for (const auto* list : {&by_title, &by_section}) {
        const QString& text = list == &by_title ? title : section;
        for (const auto& rule : *list) {
            if (rule.pattern.match(text).hasMatch()) {
                return rule.icon;
            }
        }
    }
    return fallback;
}

bool IsBound(const QWidget* widget) {
    const auto bound = [](const QWidget* source) {
        return source->property("spotlightBound").toBool();
    };
    if (qobject_cast<const QGroupBox*>(widget) != nullptr) {
        return false;
    }
    if (qobject_cast<const ConfigurationShared::Widget*>(widget) != nullptr) {
        const auto children = widget->findChildren<QWidget*>();
        return std::any_of(children.cbegin(), children.cend(), bound);
    }
    return bound(widget);
}

class VisibilityMirror final : public QObject {
public:
    VisibilityMirror(QList<QWidget*> sources_, QWidget* page_, QWidget* row_,
                     std::function<void()> changed_)
        : QObject(row_), sources{std::move(sources_)}, page{page_}, row{row_},
          changed{std::move(changed_)} {
        QSet<QWidget*> watched;
        for (auto* source : sources) {
            for (auto* widget = source; widget != nullptr && widget != page;
                 widget = widget->parentWidget()) {
                if (!watched.contains(widget)) {
                    watched.insert(widget);
                    widget->installEventFilter(this);
                }
            }
        }
        Sync();
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) {
            Sync();
        }
        return false;
    }

private:
    void Sync() {
        const bool visible = std::any_of(sources.cbegin(), sources.cend(),
                                         [this](QWidget* source) {
                                             return source->isVisibleTo(page);
                                         });
        if (row->property("sourceVisible").toBool() == visible) {
            return;
        }
        row->setProperty("sourceVisible", visible);
        changed();
    }

    QList<QWidget*> sources;
    QWidget* page;
    QWidget* row;
    std::function<void()> changed;
};

QLabel* DisplayText(const QString& text, int size) {
    auto* label = SettingsPage::Text(text, size);
    label->setFont(DisplayFont(size));
    return label;
}

bool Matches(const QString& text, const QString& query) {
    return !query.isEmpty() && text.contains(query, Qt::CaseInsensitive);
}

bool MatchesWidget(QWidget* widget, const QString& query) {
    if (Matches(widget->accessibleName(), query)) {
        return true;
    }
    for (auto* label : widget->findChildren<QLabel*>()) {
        if (Matches(label->text(), query)) {
            return true;
        }
    }
    for (auto* button : widget->findChildren<QAbstractButton*>()) {
        if (Matches(button->text(), query)) {
            return true;
        }
    }
    return false;
}

QString CleanLabel(QString text) {
    text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    text.remove(QChar::fromLatin1('&'));
    text.replace(QChar::fromLatin1('\x01'), QChar::fromLatin1('&'));
    text = text.trimmed();
    while (text.endsWith(QChar::fromLatin1(':')) || text.endsWith(QChar::fromLatin1('*')) ||
           text.endsWith(QStringLiteral("..."))) {
        text.chop(text.endsWith(QStringLiteral("...")) ? 3 : 1);
        text = text.trimmed();
    }
    return text;
}

QString PlainText(const QString& text) {
    return Qt::mightBeRichText(text) ? QTextDocumentFragment::fromHtml(text).toPlainText() : text;
}

QString Summary(const QString& tooltip) {
    QString text = PlainText(tooltip).trimmed();
    text = text.section(QChar::fromLatin1('\n'), 0, 0).trimmed();
    static const QRegularExpression old_lead(
        QStringLiteral(R"(^(?:when|if) (?:checked|enabled),\s*(?:it (?:will )?)?)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto lead = old_lead.match(text);
    if (lead.hasMatch() && lead.capturedLength() < text.size()) {
        text.remove(0, lead.capturedLength());
        text[0] = text[0].toUpper();
    }
    constexpr int limit = 150;
    if (text.size() <= limit) {
        return text;
    }
    const int sentence = text.lastIndexOf(QStringLiteral(". "), limit);
    if (sentence > 40) {
        return text.left(sentence + 1);
    }
    return text.left(limit - 1).trimmed() + QChar(0x2026);
}

QString ItemViewStyle() {
    return QStringLiteral(
               "QAbstractItemView { background: transparent; border: none; color: %1; "
               "outline: none; font-size: 13px; }"
               "QAbstractItemView::item { min-height: 34px; padding: 0 8px; border-radius: 10px; }"
               "QAbstractItemView::item:hover { background: %2; }"
               "QAbstractItemView::item:selected { background: %3; color: %1; }"
               "QHeaderView { background: transparent; border: none; }"
               "QHeaderView::section { background: transparent; color: %4; border: none; "
               "border-bottom: 1px solid %5; padding: 8px; font-size: 12px; font-weight: 600; }")
        .arg(CssColor(Color::Text()), CssColor(Color::Fill()), CssColor(Color::AccentSoft()),
             CssColor(Color::TextSecondary()), CssColor(Color::StrokeSubtle()));
}

bool IsControl(QWidget* widget) {
    return qobject_cast<QAbstractButton*>(widget) || qobject_cast<QComboBox*>(widget) ||
           qobject_cast<QAbstractSpinBox*>(widget) || qobject_cast<QLineEdit*>(widget) ||
           qobject_cast<QAbstractSlider*>(widget);
}

class GlyphButton final : public QAbstractButton {
public:
    explicit GlyphButton(const QString& glyph) {
        setText(glyph);
        setFixedSize(16, 24);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAutoRepeat(true);
        setAttribute(Qt::WA_Hover);
        setProperty("spotlightTransparent", true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setFont(Font(15, 600));
        painter.setPen(!isEnabled()   ? Color::TextFaint()
                       : underMouse() ? Color::Text()
                                      : Color::TextSecondary());
        painter.drawText(rect(), Qt::AlignCenter, text());
    }
};

class SwatchButton final : public QAbstractButton {
public:
    explicit SwatchButton(QAbstractButton* source_) : source{source_} {
        setFixedSize(34, 34);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setProperty("spotlightTransparent", true);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        painter.setPen(hasFocus() ? QPen(Color::Accent()) : QPen(Qt::NoPen));
        painter.setBrush(underMouse() ? Color::Control() : Color::Fill());
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
        const QRectF swatch = QRectF(rect()).adjusted(8, 8, -8, -8);
        QPainterPath clip;
        clip.addRoundedRect(swatch, 5, 5);
        painter.setClipPath(clip);
        const qreal ratio = devicePixelRatioF();
        painter.drawPixmap(swatch, source->icon().pixmap(swatch.size().toSize(), ratio),
                           QRectF(QPointF{}, swatch.size() * ratio));
    }

private:
    QAbstractButton* source;
};

QWidget* RadioControl(const QList<QRadioButton*>& radios) {
    for (auto* radio : radios) {
        radio->setProperty("spotlightBound", true);
    }
    QStringList names;
    for (auto* radio : radios) {
        names.append(CleanLabel(radio->text()));
    }
    auto* control = new SegmentedControl(names, SegmentedControl::Kind::Accent, 38);
    const auto sync = [control, radios] {
        const QSignalBlocker blocker(control);
        for (int i = 0; i < radios.size(); ++i) {
            if (radios[i]->isChecked()) {
                control->SetCurrentIndex(i);
            }
        }
    };
    sync();
    for (auto* radio : radios) {
        QObject::connect(radio, &QAbstractButton::toggled, control, sync);
    }
    QObject::connect(control, &SegmentedControl::CurrentChanged, control,
                     [radios, sync](int index) {
                         if (radios[index]->isEnabled()) {
                             radios[index]->click();
                         }
                         sync();
                     });
    MirrorEnabled(radios.front(), control);
    return control;
}

} // namespace

int SuggestedPresetTier(const QString& gpu_name, bool fsr4_capable) {
    static const QRegularExpression high(
        QStringLiteral(R"(RTX\s+[345]\d{3}\b|RX\s+[67][789]\d{2}\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression mid(
        QStringLiteral(
            R"(RTX\s+\d{4}\b|GTX\s+\d{4}\b|RX\s+\d{3,4}\b|Arc\s+[AB]\d{3}\b|Apple M[1-9])"),
        QRegularExpression::CaseInsensitiveOption);
    if (fsr4_capable || high.match(gpu_name).hasMatch()) {
        return 2;
    }
    return mid.match(gpu_name).hasMatch() ? 1 : 0;
}

int PerformancePresetIndex(const PerformancePreset& values) {
    for (int index = 0; index < static_cast<int>(PerformancePresets.size()); ++index) {
        if (PerformancePresets[index] == values) {
            return index;
        }
    }
    return -1;
}

int SuggestedPresetTier(const VkDeviceInfo::Record* device) {
    return device == nullptr
               ? 0
               : SuggestedPresetTier(QString::fromStdString(device->name), device->fsr4_capable);
}

QString PerformancePresetName(int index) {
    switch (index) {
    case 0:
        return SettingsPage::tr("Low-end");
    case 1:
        return SettingsPage::tr("Mid-range");
    case 2:
        return SettingsPage::tr("High-end");
    default:
        return SettingsPage::tr("Custom");
    }
}

PresetCard::PresetCard(const QString& title, QString description_, QWidget* parent)
    : QAbstractButton(parent), description{std::move(description_)} {
    setText(title);
    setCheckable(true);
    setProperty("spotlightTransparent", true);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(98);
}

void PresetCard::SetSuggestion(QString device, QString tier) {
    suggestion = std::move(device);
    suggested_tier = std::move(tier);
    setToolTip(suggestion + suggested_tier);
    update();
}

PresetCard* PresetCard::Mirror(QWidget* parent) {
    auto* copy = new PresetCard(text(), description, parent);
    copy->SetSuggestion(suggestion, suggested_tier);
    copy->setEnabled(isEnabled());
    copy->setChecked(isChecked());
    connect(this, &QAbstractButton::toggled, copy, &QAbstractButton::setChecked);
    connect(copy, &QAbstractButton::clicked, this, [this, copy] {
        click();
        copy->setChecked(isChecked());
    });
    return copy;
}

void PresetCard::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool active = isChecked();
    painter.setBrush(active         ? Color::Accent()
                     : underMouse() ? Color::Fill()
                                    : Color::FillFaint());
    painter.setPen(hasFocus() ? Color::Text()
                   : active   ? Color::Accent()
                              : Color::StrokeSubtle());
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 18, 18);

    const QColor ink = !isEnabled() ? Color::TextFaint()
                       : active     ? Color::OnAccent()
                                    : Color::Text();
    painter.setPen(ink);
    painter.setFont(Font(15, 600));
    painter.drawText(QRect(14, 13, width() - 28, 21), Qt::AlignVCenter | Qt::AlignLeft,
                     painter.fontMetrics().elidedText(text(), Qt::ElideRight, width() - 28));
    if (active) {
        painter.drawPixmap(width() - 28, 15,
                           IconPixmap(QStringLiteral("check"), 16, ink, devicePixelRatioF()));
    }
    painter.setFont(Font(12));
    painter.setPen(active ? ink : Color::TextSecondary());
    painter.drawText(QRect(14, 36, width() - 28, 34), Qt::TextWordWrap | Qt::AlignLeft,
                     description);
    if (!suggestion.isEmpty()) {
        painter.setPen(active ? ink : Color::AccentLight());
        painter.setFont(Font(12, 600));
        const int remaining =
            width() - 28 - painter.fontMetrics().horizontalAdvance(suggested_tier);
        painter.drawText(QRect(14, 69, width() - 28, 16), Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(suggestion, Qt::ElideRight, remaining) +
                             suggested_tier);
    }
}

SettingsPage::SettingsPage(QWidget* parent, bool per_game) : QWidget(parent) {
    setStyleSheet(
        QStringLiteral("QWidget[spotlightTransparent=true] { background: transparent; }"));
    setProperty("spotlightTransparent", true);
    auto* shell = new QHBoxLayout(this);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    navigation = new Surface(SettingsColor::Sidebar(), 0);
    navigation->setFixedWidth(264);
    sidebar = new QVBoxLayout(navigation);
    sidebar->setContentsMargins(14, 18, 14, 18);
    sidebar->setSpacing(2);
    shell->addWidget(navigation);

    auto* back_row = new QHBoxLayout;
    back_row->setContentsMargins(4, 0, 4, 0);
    back_row->setSpacing(8);
    auto* back = new CircleButton(QStringLiteral("caret-left"), 32);
    back->setAccessibleName(tr("Library"));
    back->setToolTip(tr("Return to the library without applying changes"));
    back_row->addWidget(back);
    back_row->addWidget(Text(tr("Library"), 13, 500, true), 1);
    sidebar->addLayout(back_row);
    connect(back, &QAbstractButton::clicked, this, &SettingsPage::BackRequested);
    sidebar->addSpacing(12);

    if (per_game) {
        auto* identity = new QHBoxLayout;
        identity->setContentsMargins(4, 0, 4, 10);
        identity->setSpacing(10);
        game_icon = new QLabel;
        game_icon->setFixedSize(44, 44);
        identity->addWidget(game_icon);
        auto* names = new QVBoxLayout;
        names->setSpacing(2);
        game_title = Text({}, 15, 600);
        game_title->setWordWrap(true);
        names->addWidget(game_title);
        names->addWidget(Text(tr("Game settings"), 12, 500, true));
        identity->addLayout(names, 1);
        sidebar->addLayout(identity);
    } else {
        auto* title = DisplayText(tr("Settings"), 22);
        title->setContentsMargins(6, 2, 6, 6);
        sidebar->addWidget(title);
    }

    search = new QLineEdit;
    search->setFont(Font(13));
    search->setClearButtonEnabled(true);
    search->setPlaceholderText(per_game ? tr("Search this game's settings")
                                        : tr("Search settings"));
    search->setAccessibleName(search->placeholderText());
    search->addAction(Icon(QStringLiteral("magnifying-glass"), Color::TextSecondary(), 16),
                      QLineEdit::LeadingPosition);
    search->setStyleSheet(
        QStringLiteral("QLineEdit { background: transparent; color: %1; border-radius: 19px; "
                       "border: 1px solid transparent; padding-left: 5px; } "
                       "QLineEdit:focus { border-color: %2; }")
            .arg(CssColor(Color::Text()), CssColor(Color::Accent())));
    auto* search_pill = new Surface(Color::Fill(), 19);
    search_pill->setFixedHeight(38);
    auto* search_layout = new QVBoxLayout(search_pill);
    search_layout->setContentsMargins(0, 0, 0, 0);
    search_layout->addWidget(search);
    sidebar->addWidget(search_pill);
    sidebar->addSpacing(8);
    auto* sidebar_drag = new WindowDragBar(navigation);
    sidebar_drag->setProperty("spotlightTransparent", true);
    sidebar->addWidget(sidebar_drag, 1);

    auto* advanced_row = new QHBoxLayout;
    advanced_row->setContentsMargins(10, 9, 10, 9);
    advanced_row->addWidget(Text(tr("Show advanced"), 13, 500, true), 1);
    advanced = new ToggleSwitch;
    advanced->setAccessibleName(tr("Show advanced"));
    advanced_row->addWidget(advanced);
    sidebar->addLayout(advanced_row);

    auto* background = new SettingsBackdrop;
    shell->addWidget(background, 1);
    main_column = new QVBoxLayout(background);
    main_column->setContentsMargins(0, 0, 0, 0);
    main_column->setSpacing(0);
    scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea { background: transparent; border: none; }"));
    auto* body = new QWidget;
    body->setProperty("spotlightTransparent", true);
    scroll->setWidget(body);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->viewport()->setProperty("spotlightTransparent", true);
    main_column->addWidget(scroll, 1);
    content = new QVBoxLayout(body);
    content->setContentsMargins(40, 0, 40, 24);
    content->setSpacing(14);
    auto* header = new QVBoxLayout;
    header->setContentsMargins(0, 0, 0, 4);
    header->setSpacing(4);
    parent_link = new QWidget;
    parent_link->setProperty("spotlightTransparent", true);
    auto* parent_row = new QHBoxLayout(parent_link);
    parent_row->setContentsMargins(0, 0, 0, 6);
    parent_row->setSpacing(8);
    auto* up = new CircleButton(QStringLiteral("caret-left"), 28);
    parent_name = Text({}, 13, 500, true);
    parent_row->addWidget(up);
    parent_row->addWidget(parent_name);
    parent_row->addStretch();
    connect(up, &QAbstractButton::clicked, this, [this] {
        if (current_category != -1 && categories[current_category].parent != -1) {
            SetCategory(categories[current_category].parent);
        }
    });
    header->addWidget(parent_link);
    parent_link->hide();
    heading = DisplayText({}, 28);
    subtitle = Text({}, 14, 500, true);
    subtitle->setWordWrap(true);
    header->addWidget(heading);
    header->addWidget(subtitle);
    auto* header_bar = new WindowDragBar(background);
    header_bar->setProperty("spotlightTransparent", true);
    header_row = new QHBoxLayout(header_bar);
    header_row->setContentsMargins(40, 22, ClientFrame ? 14 : 40, 14);
    header_row->setSpacing(12);
    header_row->addLayout(header, 1);
    if constexpr (ClientFrame) {
        header_bar->AddWindowControls(header_row);
        header_row->itemAt(header_row->count() - 1)->setAlignment(Qt::AlignTop);
        edges = new FrameEdges(this);
    }
    main_column->insertWidget(0, header_bar);
    empty = Text(tr("No matching settings"), 15, 500, true);
    empty->hide();
    content->addWidget(empty);
    content->addStretch();

    connect(search, &QLineEdit::textChanged, this, &SettingsPage::UpdateSearch);
    connect(advanced, &QAbstractButton::toggled, this, &SettingsPage::UpdateAdvanced);
    auto* focus_search = new QShortcut(QKeySequence::Find, this);
    connect(focus_search, &QShortcut::activated, search, [this] {
        search->setFocus();
        search->selectAll();
    });
}

int SettingsPage::AddCategory(const QString& title, const QString& subtitle_, const QString& icon,
                              const QColor& color, QString page_title) {
    const int index = static_cast<int>(categories.size());
    auto* button = new NavigationButton(title, icon, color);
    sidebar->insertWidget(sidebar->count() - 2, button);
    QWidget::setTabOrder(categories.isEmpty() ? static_cast<QWidget*>(search)
                                             : categories.back().button,
                         button);
    QWidget::setTabOrder(button, advanced);
    connect(button, &QAbstractButton::clicked, this, [this, index] { SetCategory(index); });

    auto* panel = new QWidget;
    panel->setProperty("spotlightTransparent", true);
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);
    content->insertWidget(content->count() - 1, panel);
    panel->hide();
    if (page_title.isEmpty()) {
        page_title = title;
    }
    Category entry;
    entry.title = title;
    entry.page_title = std::move(page_title);
    entry.subtitle = subtitle_;
    entry.icon = icon;
    entry.color = color;
    entry.button = button;
    entry.panel = panel;
    categories.push_back(std::move(entry));
    return index;
}

int SettingsPage::AddSubPage(int parent, const QString& title, const QString& subtitle_,
                             bool wide) {
    const int index = AddCategory(title, subtitle_, categories[parent].icon,
                                  categories[parent].color);
    categories[index].parent = parent;
    categories[index].wide = wide;
    categories[index].button->hide();
    return index;
}

void SettingsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (edges != nullptr) {
        edges->setGeometry(rect());
        edges->setVisible(!window()->isMaximized() && !window()->isFullScreen());
        edges->raise();
    }
}

void SettingsPage::SetBuilder(int category, std::function<void()> build) {
    categories[category].build = std::move(build);
}

QVBoxLayout* SettingsPage::AddSection(int category, const QString& title, bool advanced_) {
    auto* section = new QWidget;
    section->setProperty("spotlightTransparent", true);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    if (!title.isEmpty()) {
        auto* label = Text(title, 13, 600, true);
        label->setContentsMargins(6, 4, 6, 6);
        layout->addWidget(label);
    }
    auto* surface = new Surface(Color::FillFaint(), GroupRadius, Color::StrokeSubtle());
    surface->setObjectName(QStringLiteral("settingsGroup"));
    auto* rows = new QVBoxLayout(surface);
    rows->setContentsMargins(1, 1, 1, 1);
    rows->setSpacing(0);
    layout->addWidget(surface);
    AddBlock(category, section, title, advanced_);
    return rows;
}

void SettingsPage::AddBlock(int category, QWidget* widget, QString search_label, bool advanced_) {
    auto& entry = categories[category];
    auto* layout = static_cast<QVBoxLayout*>(entry.panel->layout());
    if (advanced_) {
        layout->addWidget(widget);
    } else {
        layout->insertWidget(entry.basic_count++, widget);
    }
    widget->setProperty("spotlightTransparent", true);
    entry.blocks.push_back({widget, std::move(search_label), advanced_});
    if (advanced_ && !advanced->isChecked() && !filtering) {
        widget->hide();
    }
}

void SettingsPage::AddPage(int category, QWidget* page, const QString& name, bool advanced_) {
    page->hide();
    auto& entry = categories[category];
    entry.pages.push_back({page, name, advanced_});
    if (advanced_ ? entry.advanced_built : entry.basic_built) {
        BuildPage(category, entry.pages.back());
        RefreshCategory(category);
    }
}

void SettingsPage::Build(int index, bool with_advanced) {
    auto& entry = categories[index];
    const bool basic_pending = !entry.basic_built;
    const bool advanced_pending = with_advanced && !entry.advanced_built;
    if (!basic_pending && !advanced_pending) {
        return;
    }
    building = true;
    if (basic_pending) {
        entry.basic_built = true;
        if (auto build = std::exchange(entry.build, {})) {
            build();
        }
        for (auto& page : categories[index].pages) {
            if (!page.advanced) {
                BuildPage(index, page);
            }
        }
    }
    if (advanced_pending) {
        categories[index].advanced_built = true;
        for (auto& page : categories[index].pages) {
            if (page.advanced) {
                BuildPage(index, page);
            }
        }
    }
    building = false;
    RefreshCategory(index);
}

void SettingsPage::BuildPage(int category, Page& page) {
    QWidget* const source = page.source;
    const bool in_advanced = page.advanced;
    const QString icon = categories[category].icon;
    const QColor color = categories[category].color;
    const auto changed = [this, category] {
        if (!building) {
            RefreshCategory(category);
        }
    };
    if (in_advanced && page.name != categories[category].title &&
        page.name != categories[category].page_title) {
        auto* heading_label = Text(page.name, 17, 600);
        heading_label->setContentsMargins(6, 10, 6, 0);
        AddBlock(category, heading_label, page.name, true);
        page.anchor = heading_label;
    }

    QVBoxLayout* rows = nullptr;
    QString section_title = in_advanced ? QString{} : page.name;
    QSet<QWidget*> consumed;
    const auto group = [&]() -> QVBoxLayout* {
        if (rows == nullptr) {
            rows = AddSection(category,
                              in_advanced && section_title == page.name ? QString{}
                                                                        : section_title,
                              in_advanced);
            if (page.anchor == nullptr) {
                page.anchor = rows->parentWidget()->parentWidget();
            }
        }
        return rows;
    };
    QHBoxLayout* toggle_box = nullptr;
    QWidget* toggle_source = nullptr;
    const auto start_section = [&](const QString& title) {
        rows = nullptr;
        section_title = title;
        toggle_box = nullptr;
    };
    const auto add_row = [&](const QString& title, const QString& tooltip,
                             const QList<QWidget*>& sources, QWidget* control) {
        toggle_box = nullptr;
        auto* layout = group();
        AddRow(layout, title.isEmpty() ? page.name : title, Summary(tooltip),
               RowIcon(title, section_title.isEmpty() ? page.name : section_title, icon), color,
               control,
               PlainText(tooltip));
        auto* row = layout->itemAt(layout->count() - 1)->widget();
        row->setProperty("sourceVisible", true);
        new VisibilityMirror(sources, source, row, changed);
    };
    const auto add_note = [&](QLabel* label) {
        toggle_box = nullptr;
        auto* note = Text(PlainText(label->text()), 12, 500, true);
        note->setWordWrap(true);
        note->setContentsMargins(18, 10, 18, 10);
        note->setObjectName(QStringLiteral("settingsRow"));
        note->setProperty("searchText", note->text());
        note->setProperty("sourceVisible", true);
        group()->addWidget(note);
        new VisibilityMirror({label}, source, note, changed);
    };
    const auto add_view = [&](QAbstractItemView* view) {
        QWidget* const origin = view->parentWidget();
        auto* block = new QWidget;
        auto* block_layout = new QVBoxLayout(block);
        block_layout->setContentsMargins(0, 0, 0, 0);
        block_layout->setSpacing(2);
        if (!section_title.isEmpty() && rows == nullptr) {
            auto* label = Text(section_title, 13, 600, true);
            label->setContentsMargins(6, 4, 6, 6);
            block_layout->addWidget(label);
        }
        auto* card = new Surface(Color::FillFaint(), GroupRadius, Color::StrokeSubtle());
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(8, 8, 8, 8);
        card_layout->setSpacing(8);
        view->setStyleSheet(ItemViewStyle());
        view->setFrameShape(QFrame::NoFrame);
        view->setMinimumHeight(260);
        card_layout->addWidget(view);
        auto* tools = new QHBoxLayout;
        tools->setContentsMargins(4, 0, 4, 2);
        tools->setSpacing(8);
        for (auto* button : origin->findChildren<QPushButton*>()) {
            if (consumed.contains(button) || button->isCheckable() ||
                !button->isVisibleTo(source) || button->text().isEmpty()) {
                continue;
            }
            consumed.insert(button);
            tools->addWidget(BindButton(button));
        }
        tools->addStretch();
        card_layout->addLayout(tools);
        block_layout->addWidget(card);
        block->setProperty("sourceVisible", true);
        AddBlock(category, block, section_title + QChar::fromLatin1('\n') + page.name,
                 in_advanced);
        if (origin != source) {
            new VisibilityMirror({origin}, source, block, changed);
        }
        start_section({});
    };

    std::function<void(QLayout*)> walk_layout;
    std::function<void(const QList<QWidget*>&)> process;
    const auto setting_row = [&](ConfigurationShared::Widget* setting) {
        if (!setting->Valid()) {
            return;
        }
        auto* control = BindSetting(setting);
        if (control == nullptr) {
            return;
        }
        const auto buttons = setting->findChildren<QPushButton*>(Qt::FindDirectChildrenOnly);
        if (std::any_of(buttons.cbegin(), buttons.cend(), [setting](QPushButton* button) {
                return button != setting->restore_button;
            })) {
            auto* box = new QWidget;
            box->setProperty("spotlightTransparent", true);
            auto* box_layout = new QHBoxLayout(box);
            box_layout->setContentsMargins(0, 0, 0, 0);
            box_layout->setSpacing(8);
            for (auto* button : buttons) {
                if (button == setting->restore_button) {
                    continue;
                }
                auto* proxy = BindButton(button);
                proxy->setProperty("sourceVisible", true);
                new VisibilityMirror({button}, setting, proxy, [proxy] {
                    proxy->setVisible(proxy->property("sourceVisible").toBool());
                });
                box_layout->addWidget(proxy);
            }
            box_layout->addWidget(control);
            control = box;
        }
        QString title;
        if (auto* label = setting->findChild<QLabel*>()) {
            title = label->text();
        }
        if (title.isEmpty() && setting->checkbox != nullptr) {
            title = setting->checkbox->text();
        }
        add_row(CleanLabel(title), setting->toolTip(), {setting}, control);
    };
    const auto bind = [&](QWidget* widget) -> QWidget* {
        if (auto* combo = qobject_cast<QComboBox*>(widget)) {
            return BindCombo(combo);
        }
        if (auto* line = qobject_cast<QLineEdit*>(widget)) {
            return BindLineEdit(line);
        }
        if (qobject_cast<QAbstractSpinBox*>(widget)) {
            return BindSpin(widget);
        }
        if (auto* slider = qobject_cast<QSlider*>(widget)) {
            return BindSlider(slider);
        }
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            if (button->isCheckable() && !qobject_cast<QRadioButton*>(button)) {
                return BindToggle(button);
            }
            return BindButton(button);
        }
        return nullptr;
    };
    const auto row_title = [](QWidget* widget) {
        if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
            if (!CleanLabel(button->text()).isEmpty()) {
                return CleanLabel(button->text());
            }
        }
        if (!widget->accessibleName().isEmpty()) {
            return widget->accessibleName();
        }
        return CleanLabel(PlainText(widget->toolTip()).section(QChar::fromLatin1('\n'), 0, 0));
    };
    process = [&](const QList<QWidget*>& widgets) {
        QLabel* label = nullptr;
        QList<QWidget*> controls;
        QList<QRadioButton*> radios;
        const auto flush = [&] {
            if (!radios.isEmpty()) {
                auto* control = RadioControl(radios);
                const QString title = label ? CleanLabel(label->text())
                                            : (section_title.isEmpty() ? page.name : section_title);
                add_row(title, label ? label->toolTip() : radios.front()->toolTip(),
                        {radios.cbegin(), radios.cend()}, control);
                radios.clear();
                label = nullptr;
            }
            if (controls.isEmpty()) {
                if (label != nullptr && !CleanLabel(label->text()).isEmpty()) {
                    add_note(label);
                }
                label = nullptr;
                return;
            }
            auto* first = controls.front();
            QWidget* control = nullptr;
            QHBoxLayout* toggle_layout = nullptr;
            QString title;
            QString tooltip;
            if (label == nullptr && controls.size() == 1 && qobject_cast<QPushButton*>(first) &&
                !static_cast<QPushButton*>(first)->isCheckable() &&
                !CleanLabel(static_cast<QPushButton*>(first)->text()).isEmpty()) {
                auto* button = static_cast<QAbstractButton*>(first);
                button->setProperty("spotlightBound", true);
                if (toggle_box != nullptr && OnSameLine(toggle_source, button)) {
                    auto* manage = Manage(CleanLabel(button->text()));
                    MirrorEnabled(button, manage);
                    connect(manage, &QAbstractButton::clicked, button, &QAbstractButton::click);
                    toggle_box->insertWidget(0, manage);
                    toggle_box = nullptr;
                    controls.clear();
                    return;
                }
                auto* manage = Manage();
                MirrorEnabled(button, manage);
                connect(manage, &QAbstractButton::clicked, button, &QAbstractButton::click);
                control = manage;
                title = CleanLabel(button->text());
                tooltip = button->toolTip();
            } else {
                auto* box = new QWidget;
                box->setProperty("spotlightTransparent", true);
                auto* box_layout = new QHBoxLayout(box);
                box_layout->setContentsMargins(0, 0, 0, 0);
                box_layout->setSpacing(8);
                for (auto* widget : controls) {
                    consumed.insert(widget);
                    if (auto* bound = bind(widget)) {
                        box_layout->addWidget(bound);
                    }
                }
                control = box;
                title = label ? CleanLabel(label->text()) : row_title(first);
                tooltip = label && !label->toolTip().isEmpty() ? label->toolTip()
                                                                : first->toolTip();
                if (controls.size() == 1 && qobject_cast<QAbstractButton*>(first) &&
                    static_cast<QAbstractButton*>(first)->isCheckable()) {
                    toggle_layout = box_layout;
                }
            }
            QList<QWidget*> sources = controls;
            add_row(title, tooltip, sources, control);
            if (toggle_layout != nullptr) {
                toggle_box = toggle_layout;
                toggle_source = first;
            }
            controls.clear();
            label = nullptr;
        };
        for (auto* widget : widgets) {
            if (consumed.contains(widget)) {
                continue;
            }
            if (IsBound(widget)) {
                if (controls.isEmpty() && radios.isEmpty()) {
                    label = nullptr;
                }
                continue;
            }
            if (auto* setting = qobject_cast<ConfigurationShared::Widget*>(widget)) {
                flush();
                setting_row(setting);
            } else if (auto* text = qobject_cast<QLabel*>(widget)) {
                if (text->text().isEmpty()) {
                    continue;
                }
                flush();
                label = text;
            } else if (auto* box = qobject_cast<QGroupBox*>(widget)) {
                flush();
                start_section(CleanLabel(box->title()));
                if (box->isCheckable() && !box->property("spotlightBound").toBool()) {
                    add_row(CleanLabel(box->title()), box->toolTip(), {box}, BindToggle(box));
                }
                if (box->layout() != nullptr) {
                    walk_layout(box->layout());
                }
                start_section({});
            } else if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
                flush();
                for (int i = 0; i < tabs->count(); ++i) {
                    start_section(CleanLabel(tabs->tabText(i)));
                    process({tabs->widget(i)});
                }
                start_section({});
            } else if (auto* area = qobject_cast<QScrollArea*>(widget)) {
                flush();
                if (area->widget() != nullptr && area->widget()->layout() != nullptr) {
                    walk_layout(area->widget()->layout());
                } else if (area->layout() != nullptr) {
                    walk_layout(area->layout());
                }
            } else if (auto* view = qobject_cast<QAbstractItemView*>(widget)) {
                flush();
                add_view(view);
            } else if (auto* radio = qobject_cast<QRadioButton*>(widget)) {
                if (!controls.isEmpty()) {
                    flush();
                }
                radios.append(radio);
            } else if (auto* check = qobject_cast<QCheckBox*>(widget)) {
                if (label == nullptr || !controls.isEmpty()) {
                    flush();
                    auto* box = new QWidget;
                    box->setProperty("spotlightTransparent", true);
                    auto* box_layout = new QHBoxLayout(box);
                    box_layout->setContentsMargins(0, 0, 0, 0);
                    box_layout->setSpacing(8);
                    auto* toggle = BindToggle(check);
                    toggle->setAccessibleName(CleanLabel(check->text()));
                    box_layout->addWidget(toggle);
                    add_row(CleanLabel(check->text()), check->toolTip(), {check}, box);
                    toggle_box = box_layout;
                    toggle_source = check;
                } else {
                    controls.append(check);
                }
            } else if (IsControl(widget)) {
                if (!radios.isEmpty()) {
                    flush();
                }
                controls.append(widget);
            } else if (widget->layout() != nullptr) {
                flush();
                walk_layout(widget->layout());
            }
        }
        flush();
    };

    std::function<void(QLayout*, QList<QWidget*>&)> collect = [&](QLayout* layout,
                                                                QList<QWidget*>& out) {
        for (int i = 0; i < layout->count(); ++i) {
            auto* item = layout->itemAt(i);
            if (auto* widget = item->widget()) {
                out.append(widget);
            } else if (auto* nested = item->layout()) {
                collect(nested, out);
            }
        }
    };
    walk_layout = [&](QLayout* layout) {
        if (auto* grid = qobject_cast<QGridLayout*>(layout)) {
            std::map<int, std::vector<std::pair<int, QLayoutItem*>>> grid_rows;
            for (int index = 0; index < grid->count(); ++index) {
                int row = 0;
                int column = 0;
                int row_span = 0;
                int column_span = 0;
                grid->getItemPosition(index, &row, &column, &row_span, &column_span);
                grid_rows[row].emplace_back(column, grid->itemAt(index));
            }
            for (auto& [row, items] : grid_rows) {
                std::stable_sort(items.begin(), items.end(),
                                 [](const auto& a, const auto& b) { return a.first < b.first; });
                QList<QWidget*> unit;
                for (const auto& [column, item] : items) {
                    if (auto* widget = item->widget()) {
                        unit.append(widget);
                    } else if (auto* nested = item->layout()) {
                        collect(nested, unit);
                    }
                }
                process(unit);
            }
            return;
        }
        if (auto* form = qobject_cast<QFormLayout*>(layout)) {
            for (int row = 0; row < form->rowCount(); ++row) {
                QList<QWidget*> unit;
                for (auto role : {QFormLayout::LabelRole, QFormLayout::FieldRole,
                                  QFormLayout::SpanningRole}) {
                    if (auto* item = form->itemAt(row, role)) {
                        if (auto* widget = item->widget()) {
                            unit.append(widget);
                        } else if (auto* nested = item->layout()) {
                            collect(nested, unit);
                        }
                    }
                }
                process(unit);
            }
            return;
        }
        auto* box = qobject_cast<QBoxLayout*>(layout);
        if (box != nullptr && (box->direction() == QBoxLayout::LeftToRight ||
                               box->direction() == QBoxLayout::RightToLeft)) {
            QList<QWidget*> unit;
            collect(box, unit);
            process(unit);
            return;
        }
        for (int i = 0; i < layout->count(); ++i) {
            auto* item = layout->itemAt(i);
            if (auto* widget = item->widget()) {
                process({widget});
            } else if (auto* nested = item->layout()) {
                walk_layout(nested);
            }
        }
    };
    if (source->layout() != nullptr) {
        walk_layout(source->layout());
    }
}

void SettingsPage::SetCategory(int category) {
    if (category < 0 || category >= categories.size()) {
        return;
    }
    if (filtering && categories[category].button->isHidden()) {
        search->clear();
    }
    Build(category, advanced->isChecked() || filtering);
    if (current_category != -1 && current_category != category) {
        categories[current_category].panel->hide();
        const int previous_parent = categories[current_category].parent;
        categories[previous_parent == -1 ? current_category : previous_parent].button->setChecked(
            false);
    }
    current_category = category;
    const auto& entry = categories[category];
    entry.panel->show();
    categories[entry.parent == -1 ? category : entry.parent].button->setChecked(true);
    parent_link->setVisible(entry.parent != -1);
    navigation->setVisible(!entry.wide);
    if (banner != nullptr) {
        banner->setVisible(
            banner_categories.contains(entry.parent == -1 ? category : entry.parent));
    }
    content->setContentsMargins(entry.wide ? 24 : 40, 0, entry.wide ? 24 : 40, 24);
    header_row->setContentsMargins(entry.wide ? 24 : 40, 22, ClientFrame ? 14 : 40, 14);
    scroll->setHorizontalScrollBarPolicy(entry.wide ? Qt::ScrollBarAsNeeded
                                                    : Qt::ScrollBarAlwaysOff);
    if (entry.parent != -1) {
        parent_name->setText(categories[entry.parent].title);
    }
    heading->setText(entry.page_title);
    subtitle->setText(entry.subtitle);
    subtitle->setVisible(!entry.subtitle.isEmpty());
    empty->hide();
    scroll->verticalScrollBar()->setValue(0);
}

void SettingsPage::ShowAdvancedPage(int category, QWidget* page) {
    search->clear();
    SetCategory(category);
    advanced->setChecked(true);
    for (const auto& entry : categories[category].pages) {
        if (entry.source != page || entry.anchor == nullptr) {
            continue;
        }
        QTimer::singleShot(0, scroll, [this, anchor = entry.anchor] {
            scroll->verticalScrollBar()->setValue(anchor->mapTo(scroll->widget(), QPoint{}).y() -
                                                  content->contentsMargins().top());
        });
        return;
    }
}

void SettingsPage::SetFooter(QWidget* widget) {
    widget->setProperty("spotlightTransparent", true);
    main_column->addWidget(widget);
}

void SettingsPage::SetBanner(QWidget* widget, QList<int> shown_in) {
    banner = widget;
    banner_categories = std::move(shown_in);
    banner->setProperty("spotlightTransparent", true);
    content->insertWidget(0, banner);
}

void SettingsPage::SetGame(const QString& title, const QPixmap& icon) {
    game_title->setText(title);
    game_title->setToolTip(title);
    game_icon->setPixmap(WholeIcon(icon, 44, devicePixelRatioF()));
}

void SettingsPage::UpdateAdvanced() {
    if (current_category == -1) {
        return;
    }
    if (advanced->isChecked()) {
        Build(current_category, true);
    }
    RefreshCategory(current_category);
}

bool SettingsPage::RefreshCategory(int index) {
    const auto source_visible = [](const QWidget* widget) {
        const auto value = widget->property("sourceVisible");
        return !value.isValid() || value.toBool();
    };
    const auto& entry = categories[index];
    const QString query = search->text().trimmed();
    const bool category_match = Matches(entry.title, query);
    bool any = false;
    for (const auto& block : entry.blocks) {
        if (!source_visible(block.widget) ||
            (block.advanced && !advanced->isChecked() && !filtering)) {
            block.widget->hide();
            continue;
        }
        const bool label_match = !filtering || category_match || Matches(block.search_label, query);
        bool visible = false;
        const auto rows = block.widget->findChildren<QWidget*>(QStringLiteral("settingsRow"));
        if (rows.isEmpty()) {
            visible = label_match || MatchesWidget(block.widget, query);
        } else {
            for (auto* row : rows) {
                const bool shown =
                    source_visible(row) &&
                    (label_match || Matches(row->property("searchText").toString(), query));
                row->setVisible(shown);
                visible |= shown;
            }
        }
        block.widget->setVisible(visible);
        any |= visible;
    }
    return any;
}

void SettingsPage::UpdateSearch() {
    const QString query = search->text().trimmed();
    if (!query.isEmpty() && !filtering) {
        unfiltered_category = current_category;
    }
    filtering = !query.isEmpty();
    int first = -1;
    for (int i = 0; i < categories.size(); ++i) {
        if (filtering) {
            Build(i, true);
        }
        const bool any = RefreshCategory(i) && filtering;
        auto& entry = categories[i];
        entry.button->setVisible(filtering ? any : entry.parent == -1);
        if (any && first == -1) {
            first = i;
        }
    }
    if (!filtering) {
        if (unfiltered_category != -1) {
            SetCategory(std::exchange(unfiltered_category, -1));
        }
        return;
    }
    if (first == -1) {
        if (current_category != -1) {
            categories[current_category].panel->hide();
        }
        parent_link->hide();
        heading->setText(tr("Search results"));
        subtitle->hide();
        empty->show();
        return;
    }
    if (current_category == -1 || categories[current_category].button->isHidden()) {
        SetCategory(first);
    } else {
        SetCategory(current_category);
    }
}

QLabel* SettingsPage::Text(const QString& text, int size, int weight, bool secondary) {
    auto* label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setFont(Font(size, weight));
    label->setStyleSheet(
        QStringLiteral("QLabel { background: transparent; border: none; color: %1; }")
            .arg(CssColor(secondary ? Color::TextSecondary() : Color::Text())));
    return label;
}

QPixmap SettingsPage::WholeIcon(const QPixmap& icon, int size, qreal dpr) {
    return WholeIcon(icon, QSize(size, size), dpr);
}

QPixmap SettingsPage::WholeIcon(const QPixmap& icon, QSize size, qreal dpr) {
    const qreal radius = size.width() > size.height() ? 10 : 9;
    QPixmap result(size * dpr);
    result.setDevicePixelRatio(dpr);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    if (icon.isNull()) {
        const int glyph = std::min(size.width(), size.height()) / 2;
        painter.setPen(Qt::NoPen);
        painter.setBrush(Color::Fill());
        painter.drawRoundedRect(QRectF(QPointF(), size), radius, radius);
        painter.drawPixmap(
            (size.width() - glyph) / 2, (size.height() - glyph) / 2,
            IconPixmap(QStringLiteral("game-controller"), glyph, Color::TextMuted(), dpr));
        return result;
    }
    const QSizeF image_size = icon.size().scaled(size * dpr, Qt::KeepAspectRatio) / dpr;
    const QRectF target((size.width() - image_size.width()) / 2,
                        (size.height() - image_size.height()) / 2, image_size.width(),
                        image_size.height());
    QPainterPath path;
    path.addRoundedRect(target, radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(target, icon, icon.rect());
    return result;
}

QWidget* SettingsPage::Card(const QString& icon, int image_size) {
    if (image_size > 0) {
        auto* tile = new Surface(Color::Fill(), Radius::Card);
        tile->setFixedSize(image_size, image_size);
        auto* layout = new QVBoxLayout(tile);
        auto* label = new QLabel;
        label->setPixmap(
            IconPixmap(icon, image_size / 2, Color::Text(), tile->devicePixelRatioF()));
        layout->addWidget(label, 0, Qt::AlignCenter);
        return tile;
    }
    return new Surface(Color::FillFaint(), Radius::Card, Color::StrokeSubtle());
}

QWidget* SettingsPage::Badge(const QString& text, const QColor& fill, const QColor& ink) {
    auto* badge = new Surface(fill, 12);
    auto* layout = new QHBoxLayout(badge);
    layout->setContentsMargins(10, 0, 10, 0);
    auto* label = Text(text, 12, 600);
    label->setStyleSheet(
        QStringLiteral("QLabel { background: transparent; color: %1; }").arg(CssColor(ink)));
    layout->addWidget(label, 0, Qt::AlignVCenter);
    badge->setFixedHeight(24);
    badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return badge;
}

QLabel* SettingsPage::AddRow(QVBoxLayout* group, const QString& title, const QString& description,
                             const QString& icon, const QColor& color, QWidget* control,
                             const QString& tooltip, const QPixmap& image) {
    auto* row = new SettingsRow(group->count() > 0);
    row->setObjectName(QStringLiteral("settingsRow"));
    row->setProperty("spotlightTransparent", true);
    row->setProperty("searchText", QString(title + QChar::fromLatin1('\n') + description +
                                           QChar::fromLatin1('\n') + tooltip));
    if (!tooltip.isEmpty() && tooltip != description) {
        row->setToolTip(tooltip);
    }
    row->setMinimumHeight(72);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(14, 12, 18, 12);
    layout->setSpacing(14);
    if (image.isNull()) {
        layout->addWidget(new IconTile(icon, color), 0, Qt::AlignVCenter);
    } else {
        const QSize size = image.width() > image.height() ? QSize(96, 54) : QSize(40, 40);
        auto* picture = new QLabel;
        picture->setFixedSize(size);
        picture->setPixmap(WholeIcon(image, size, row->devicePixelRatioF()));
        layout->addWidget(picture, 0, Qt::AlignVCenter);
    }
    auto* copy = new QVBoxLayout;
    copy->setSpacing(3);
    auto* title_line = new QHBoxLayout;
    title_line->setSpacing(8);
    auto* name = Text(title, 15, 600);
    title_line->addWidget(name);
    auto* detail = new DescriptionLabel;
    detail->setText(description);
    detail->setTextFormat(Qt::PlainText);
    detail->setFont(Font(13, 400));
    detail->setStyleSheet(
        QStringLiteral("QLabel { background: transparent; border: none; color: %1; }")
            .arg(CssColor(Color::TextSecondary())));
    detail->setWordWrap(true);
    detail->setMaximumWidth(400);
    copy->addLayout(title_line);
    copy->addWidget(detail);
    layout->addLayout(copy, 1);
    copy->setAlignment(Qt::AlignVCenter);
    if (control != nullptr) {
        if (control->metaObject() == &QWidget::staticMetaObject) {
            control->setProperty("spotlightTransparent", true);
        }
        if (auto* badge = control->findChild<QWidget*>(QStringLiteral("changedBadge"))) {
            title_line->addWidget(badge, 0, Qt::AlignVCenter);
        }
        if (auto* badge = control->findChild<QWidget*>(QStringLiteral("titleBadge"))) {
            title_line->addWidget(badge, 0, Qt::AlignVCenter);
        }
        if (auto* line = control->findChild<QWidget*>(QStringLiteral("detailLine"))) {
            copy->addWidget(line);
        }
        control->setAccessibleName(title);
        layout->addWidget(control);
    }
    title_line->addStretch();
    group->addWidget(row);
    detail->setVisible(!description.isEmpty());
    return detail;
}

QAbstractButton* SettingsPage::Manage(const QString& text) {
    return new ManageButton(text);
}

ConfigurationShared::Widget* SettingsPage::Editor(QWidget* page,
                                                  const Settings::BasicSetting& setting) {
    for (auto* editor : page->findChildren<ConfigurationShared::Widget*>()) {
        if (editor->Valid() && editor->SettingId() == setting.Id()) {
            return editor;
        }
    }
    return nullptr;
}

QString SettingsPage::ClosedSettingLabel(const QComboBox* source, u32 setting_id, int index) {
    const auto value = source->itemData(index).toUInt();
    if (setting_id == Settings::values.resolution_setup.Id()) {
        Settings::ResolutionScalingInfo info{};
        Settings::TranslateResolutionInfo(static_cast<Settings::ResolutionSetup>(value), info);
        return tr("%1×").arg(info.up_factor);
    } else if (setting_id == Settings::values.scaling_filter.Id()) {
        switch (static_cast<Settings::ScalingFilter>(value)) {
        case Settings::ScalingFilter::Fsr:
            return tr("FSR 1");
        case Settings::ScalingFilter::Fsr3:
            return tr("FSR 1 Soft");
        case Settings::ScalingFilter::Sgsr:
            return tr("SGSR");
        case Settings::ScalingFilter::SgsrEdge:
            return tr("SGSR EdgeDir");
        default:
            break;
        }
    }
    return source->itemText(index);
}

QComboBox* SettingsPage::BindCombo(QComboBox* source, const QString& placeholder,
                                   std::function<QString(const QComboBox&, int)> closed_label) {
    source->setProperty("spotlightBound", true);
    auto* proxy = new ComboPill(std::move(closed_label));
    proxy->setModel(source->model());
    proxy->setPlaceholderText(placeholder);
    const auto sync = [source, proxy] {
        if (proxy->currentIndex() != source->currentIndex()) {
            const QSignalBlocker blocker(proxy);
            proxy->setCurrentIndex(source->currentIndex());
        }
        proxy->Refresh();
    };
    MirrorEnabled(source, proxy);
    sync();
    connect(source, &QComboBox::currentIndexChanged, proxy, sync);
    connect(source, &QComboBox::currentTextChanged, proxy, sync);
    connect(proxy, &QComboBox::activated, source,
            [source](int index) { SetComboIndex(source, index); });
    return proxy;
}

QWidget* SettingsPage::BindFrameGeneration(QWidget* graphics) {
    const auto& settings = Settings::values;
    auto* frame = Editor(graphics, settings.frame_gen);
    if (frame == nullptr) {
        return nullptr;
    }
    auto* type_editor = Editor(graphics, settings.frame_gen_type);
    auto* multiplier_editor = Editor(graphics, settings.frame_gen_multiplier);
    if (frame->restore_button || !type_editor || !type_editor->combobox || !multiplier_editor ||
        !multiplier_editor->spinbox) {
        return BindSetting(frame);
    }
    auto* enabled = frame->checkbox;
    auto* method = type_editor->combobox;
    auto* multiplier = multiplier_editor->spinbox;
    if (!method->isEnabled() || !multiplier->isEnabled() || method->count() == 0) {
        return BindSetting(frame);
    }
    struct Choice {
        int method;
        int multiplier;
    };
    QList<Choice> choices{{-1, 0}};
    QStringList names{tr("Off")};
    QStringList short_names{tr("Off")};
    for (int i = 0; i < method->count(); ++i) {
        if (!(method->model()->flags(method->model()->index(i, method->modelColumn())) &
              Qt::ItemIsEnabled)) {
            continue;
        }
        QString short_name;
        switch (static_cast<Settings::FrameGenType>(method->itemData(i).toUInt())) {
        case Settings::FrameGenType::Lsfg:
            short_name = tr("LSFG");
            break;
        case Settings::FrameGenType::OpticalFlow:
            short_name = tr("Optical Flow");
            break;
        case Settings::FrameGenType::NvidiaDlssG:
            short_name = tr("DLSS FG");
            break;
        case Settings::FrameGenType::FlyHigh:
            short_name = tr("FlyHigh");
            break;
        }
        for (int rate = multiplier->minimum(); rate <= multiplier->maximum(); ++rate) {
            choices.append({i, rate});
            names.append(tr("%1 %2×").arg(method->itemText(i)).arg(rate));
            short_names.append(tr("%1 %2×").arg(short_name).arg(rate));
        }
    }
    if (choices.size() < 2) {
        return BindSetting(frame);
    }
    auto* combo =
        new ComboPill([short_names](const QComboBox&, int i) { return short_names.value(i); });
    combo->setPlaceholderText(tr("On"));
    combo->addItems(names);
    MirrorEnabled(enabled, combo);
    const auto sync = [combo, enabled, method, multiplier, choices] {
        int index = -1;
        if (!enabled->isChecked()) {
            index = 0;
        } else {
            for (int i = 1; i < choices.size(); ++i) {
                if (choices[i].method == method->currentIndex() &&
                    choices[i].multiplier == multiplier->value()) {
                    index = i;
                    break;
                }
            }
        }
        combo->setCurrentIndex(index);
        combo->Refresh();
    };
    connect(combo, &QComboBox::activated, combo,
            [enabled, method, multiplier, choices, sync](int i) {
                const Choice choice = choices.at(i);
                if (i > 0) {
                    SetComboIndex(method, choice.method);
                    multiplier->setValue(choice.multiplier);
                }
                if (enabled->isChecked() != (i > 0)) {
                    enabled->click();
                }
                sync();
            });
    connect(enabled, &QCheckBox::toggled, combo, sync);
    connect(method, &QComboBox::currentIndexChanged, combo, sync);
    connect(multiplier, &QSpinBox::valueChanged, combo, sync);
    sync();
    return combo;
}

void SettingsPage::SetComboIndex(QComboBox* source, int index) {
    if (source->isEnabled() && index >= 0 && index < source->count()) {
        source->setCurrentIndex(index);
        emit source->activated(index);
    }
}

QWidget* SettingsPage::BindToggle(QAbstractButton* source) {
    source->setProperty("spotlightBound", true);
    auto* toggle = new ToggleSwitch;
    toggle->setChecked(source->isChecked());
    MirrorEnabled(source, toggle);
    connect(source, &QAbstractButton::toggled, toggle, &QAbstractButton::setChecked);
    connect(toggle, &QAbstractButton::clicked, source, [source](bool checked) {
        if (source->isChecked() != checked) {
            source->click();
        }
    });
    return toggle;
}

QWidget* SettingsPage::BindToggle(QGroupBox* source) {
    source->setProperty("spotlightBound", true);
    auto* toggle = new ToggleSwitch;
    toggle->setChecked(source->isChecked());
    MirrorEnabled(source, toggle);
    connect(source, &QGroupBox::toggled, toggle, &QAbstractButton::setChecked);
    connect(toggle, &QAbstractButton::toggled, source, &QGroupBox::setChecked);
    return toggle;
}

void SettingsPage::WatchOverride(ConfigurationShared::Widget* source, QWidget* context,
                                 std::function<void()> update) {
    new WatchWidget(source, context, QEvent::DynamicPropertyChange, std::move(update));
}

QWidget* SettingsPage::BindSetting(ConfigurationShared::Widget* source) {
    QWidget* editor = nullptr;
    if (source->combobox != nullptr) {
        editor = BindCombo(source->combobox, {},
                           [id = source->SettingId()](const QComboBox& combo, int index) {
                               return ClosedSettingLabel(&combo, id, index);
                           });
    } else if (auto* slider = source->slider) {
        const auto labels = source->findChildren<QLabel*>();
        QLabel* feedback = labels.size() >= 2 ? labels.back() : nullptr;
        auto* value = Text(feedback ? feedback->text() : QString::number(slider->value()), 13, 600);
        value->setMinimumWidth(44);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        connect(slider, &QSlider::valueChanged, value, [slider, value, feedback] {
            value->setText(feedback ? feedback->text() : QString::number(slider->value()));
        });
        editor = new QWidget;
        editor->setProperty("spotlightTransparent", true);
        auto* horizontal = new QHBoxLayout(editor);
        horizontal->setContentsMargins(0, 0, 0, 0);
        horizontal->setSpacing(6);
        horizontal->addWidget(BindSlider(slider));
        horizontal->addWidget(value);
    } else if (source->spinbox != nullptr || source->double_spinbox != nullptr ||
               source->date_time_edit != nullptr) {
        QWidget* spin = source->spinbox;
        if (spin == nullptr) {
            spin = source->double_spinbox;
        }
        if (spin == nullptr) {
            spin = source->date_time_edit;
        }
        editor = BindSpin(spin);
        if (source->SettingId() == Settings::values.fps_limit.Id() &&
            source->spinbox->specialValueText().isEmpty()) {
            editor->findChild<QSpinBox*>()->setSpecialValueText(tr("Auto"));
        }
    } else if (source->line_edit != nullptr) {
        editor = BindLineEdit(source->line_edit);
    } else if (!source->radio_buttons.empty()) {
        QList<QRadioButton*> radios;
        for (const auto& [value, radio] : source->radio_buttons) {
            radios.append(radio);
        }
        editor = RadioControl(radios);
    }
    if (source->checkbox != nullptr) {
        auto* toggle = BindToggle(source->checkbox);
        if (editor == nullptr) {
            editor = toggle;
        } else {
            auto* pair = new QWidget;
            pair->setProperty("spotlightTransparent", true);
            auto* layout = new QHBoxLayout(pair);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(12);
            layout->addWidget(editor);
            layout->addWidget(toggle);
            editor = pair;
        }
    }
    if (editor == nullptr || source->restore_button == nullptr) {
        return editor;
    }
    auto* combined = new QWidget;
    combined->setProperty("spotlightTransparent", true);
    auto* layout = new QHBoxLayout(combined);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto* badge = new Surface(Color::Accent(), 9);
    badge->setObjectName(QStringLiteral("changedBadge"));
    auto* badge_layout = new QHBoxLayout(badge);
    badge_layout->setContentsMargins(8, 0, 8, 0);
    auto* badge_text = Text(tr("Changed"), 11, 600);
    badge_text->setStyleSheet(QStringLiteral("QLabel { background: transparent; color: %1; }")
                                  .arg(CssColor(Color::OnAccent())));
    badge_layout->addWidget(badge_text);
    badge->setFixedHeight(19);
    badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* reset = new CircleButton(QStringLiteral("arrow-counter-clockwise"), 32);
    reset->setAccessibleName(tr("Use global setting"));
    reset->setToolTip(reset->accessibleName());
    reset->setFocusPolicy(Qt::StrongFocus);
    connect(reset, &QAbstractButton::clicked, source->restore_button, &QAbstractButton::click);
    WatchOverride(source, combined, [source, badge, reset] {
        const bool custom = source->property("custom").toBool();
        badge->setVisible(custom);
        reset->setVisible(custom);
    });
    MirrorEnabled(source, reset);
    layout->addWidget(badge);
    layout->addWidget(editor);
    layout->addWidget(reset);
    return combined;
}

QWidget* SettingsPage::BindSpin(QWidget* source) {
    source->setProperty("spotlightBound", true);
    QAbstractSpinBox* proxy = nullptr;
    if (auto* spin = qobject_cast<QSpinBox*>(source)) {
        auto* copy = new QSpinBox;
        copy->setRange(spin->minimum(), spin->maximum());
        copy->setSingleStep(spin->singleStep());
        copy->setPrefix(spin->prefix());
        copy->setSuffix(spin->suffix().isEmpty() || !spin->suffix().front().isLetter()
                            ? spin->suffix()
                            : QChar::fromLatin1(' ') + spin->suffix());
        copy->setSpecialValueText(spin->specialValueText());
        copy->setDisplayIntegerBase(spin->displayIntegerBase());
        copy->setValue(spin->value());
        connect(spin, &QSpinBox::valueChanged, copy, &QSpinBox::setValue);
        connect(copy, &QSpinBox::valueChanged, spin, &QSpinBox::setValue);
        proxy = copy;
    } else if (auto* real = qobject_cast<QDoubleSpinBox*>(source)) {
        auto* copy = new QDoubleSpinBox;
        copy->setDecimals(real->decimals());
        copy->setRange(real->minimum(), real->maximum());
        copy->setSingleStep(real->singleStep());
        copy->setPrefix(real->prefix());
        copy->setSuffix(real->suffix());
        copy->setSpecialValueText(real->specialValueText());
        copy->setValue(real->value());
        connect(real, &QDoubleSpinBox::valueChanged, copy, &QDoubleSpinBox::setValue);
        connect(copy, &QDoubleSpinBox::valueChanged, real, &QDoubleSpinBox::setValue);
        proxy = copy;
    } else if (auto* date = qobject_cast<QDateTimeEdit*>(source)) {
        auto* copy = new QDateTimeEdit;
        copy->setDisplayFormat(date->displayFormat());
        copy->setDateTimeRange(date->minimumDateTime(), date->maximumDateTime());
        copy->setDateTime(date->dateTime());
        connect(date, &QDateTimeEdit::dateTimeChanged, copy, &QDateTimeEdit::setDateTime);
        connect(copy, &QDateTimeEdit::dateTimeChanged, date, &QDateTimeEdit::setDateTime);
        proxy = copy;
    }
    if (proxy == nullptr) {
        return nullptr;
    }
    auto* field = NumberField(proxy);
    MirrorEnabled(source, field);
    return field;
}

QWidget* SettingsPage::BindSlider(QSlider* source) {
    source->setProperty("spotlightBound", true);
    auto* proxy = new SlimSlider;
    proxy->setRange(source->minimum(), source->maximum());
    proxy->setSingleStep(source->singleStep());
    proxy->setPageStep(source->pageStep());
    proxy->setInvertedAppearance(source->invertedAppearance());
    proxy->setValue(source->value());
    connect(source, &QSlider::valueChanged, proxy, &QSlider::setValue);
    connect(proxy, &QSlider::valueChanged, source, [source](int next) {
        if (source->value() != next) {
            source->setValue(next);
            emit source->actionTriggered(QAbstractSlider::SliderMove);
        }
    });
    auto* minus = new GlyphButton(QStringLiteral("\u2212"));
    auto* plus = new GlyphButton(QStringLiteral("+"));
    connect(minus, &QAbstractButton::clicked, proxy,
            [proxy] { proxy->triggerAction(QAbstractSlider::SliderSingleStepSub); });
    connect(plus, &QAbstractButton::clicked, proxy,
            [proxy] { proxy->triggerAction(QAbstractSlider::SliderSingleStepAdd); });
    auto* control = new QWidget;
    control->setProperty("spotlightTransparent", true);
    auto* layout = new QHBoxLayout(control);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(minus);
    layout->addWidget(proxy);
    layout->addWidget(plus);
    MirrorEnabled(source, control);
    return control;
}

QWidget* SettingsPage::BindLineEdit(QLineEdit* source) {
    source->setProperty("spotlightBound", true);
    auto* edit = new QLineEdit;
    edit->setFont(Font(13));
    edit->setFrame(false);
    edit->setStyleSheet(FieldStyle(QStringLiteral("QLineEdit")));
    edit->setPlaceholderText(source->placeholderText());
    edit->setEchoMode(source->echoMode());
    edit->setReadOnly(source->isReadOnly());
    edit->setMaxLength(source->maxLength());
    edit->setInputMask(source->inputMask());
    edit->setValidator(source->validator());
    edit->setText(source->text());
    edit->setCursorPosition(0);
    connect(source, &QLineEdit::textChanged, edit, [edit](const QString& text) {
        if (edit->text() != text) {
            edit->setText(text);
            edit->setCursorPosition(0);
        }
    });
    connect(edit, &QLineEdit::textEdited, source, [source](const QString& text) {
        source->setText(text);
        emit source->textEdited(text);
    });
    connect(edit, &QLineEdit::editingFinished, source, &QLineEdit::editingFinished);
    connect(edit, &QLineEdit::returnPressed, source, &QLineEdit::returnPressed);
    auto* field = new FieldPill(edit);
    auto* layout = new QHBoxLayout(field);
    layout->setContentsMargins(14, 0, 14, 0);
    layout->addWidget(edit);
    field->setMinimumWidth(220);
    field->setMaximumWidth(320);
    field->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    MirrorEnabled(source, field);
    return field;
}

QAbstractButton* SettingsPage::BindButton(QAbstractButton* source, const QString& text) {
    source->setProperty("spotlightBound", true);
    const QString label = text.isEmpty() ? CleanLabel(source->text()) : text;
    QAbstractButton* proxy = nullptr;
    if (!label.isEmpty()) {
        proxy = new PillButton(label);
    } else if (!source->icon().isNull()) {
        proxy = new SwatchButton(source);
    } else {
        proxy = new CircleButton(QStringLiteral("folder-open"), 34);
    }
    proxy->setFocusPolicy(Qt::StrongFocus);
    proxy->setToolTip(source->toolTip());
    proxy->setAccessibleName(label.isEmpty() ? source->toolTip() : label);
    MirrorEnabled(source, proxy);
    connect(proxy, &QAbstractButton::clicked, source, [source, proxy, text] {
        source->click();
        if (text.isEmpty() && !CleanLabel(source->text()).isEmpty() &&
            proxy->text() != CleanLabel(source->text())) {
            proxy->setText(CleanLabel(source->text()));
            proxy->updateGeometry();
        }
        proxy->update();
    });
    return proxy;
}

} // namespace Spotlight
