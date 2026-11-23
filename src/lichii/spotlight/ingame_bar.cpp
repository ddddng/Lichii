// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/spotlight/ingame_bar.h"

#include <cmath>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

#include "lichii/spotlight/shader_chip.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace Spotlight {
namespace {

class ConfigurationButton final : public QAbstractButton {
public:
    explicit ConfigurationButton(QWidget* parent) : QAbstractButton(parent) {
        setFont(Font(12, 600));
        setFixedHeight(26);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setCheckable(true);
    }

    QSize sizeHint() const override {
        return {fontMetrics().horizontalAdvance(text()) + 54, 26};
    }

protected:
    void nextCheckState() override {}

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(hasFocus() || isChecked() ? QPen(Color::Accent()) : Qt::NoPen);
        painter.setBrush(isChecked()                ? Color::AccentSoft()
                         : underMouse() || isDown() ? Color::ControlHover()
                                                    : Color::Fill());
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 13, 13);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Color::Success());
        painter.drawEllipse(QRect{10, 9, 8, 8});
        painter.setFont(font());
        painter.setPen(Color::Text());
        painter.drawText(QRect{24, 0, width() - 54, height()}, Qt::AlignVCenter, text());
        painter.drawPixmap(
            width() - 24, 5,
            IconPixmap(u"caret-down", 16, Color::TextSecondary(), devicePixelRatioF()));
    }

    void enterEvent(QEnterEvent*) override {
        update();
    }
    void leaveEvent(QEvent*) override {
        update();
    }
};

} // namespace

class BarChip final : public QAbstractButton {
public:
    BarChip(const QString& text, QString icon, int padding, QWidget* parent)
        : QAbstractButton(parent), icon_name(std::move(icon)), padding(padding) {
        setText(text);
        setFont(Font(12, 700));
        setFixedHeight(26);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
    }

    void SetInk(const QColor& color) {
        ink = color;
        update();
    }

    void SetAccent(bool on) {
        accent = on;
        update();
    }

    QSize sizeHint() const override {
        return {2 * padding + 16 + 6 + fontMetrics().horizontalAdvance(text()), 26};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool hovered = underMouse() || isDown();
        const QColor fill = accent    ? (hovered ? Color::AccentLight() : Color::Accent())
                            : hovered ? Color::ControlHover()
                                      : Color::Fill();
        const QColor foreground = accent ? Color::OnAccent() : ink;
        painter.setPen(hasFocus() ? QPen(accent ? Color::Text() : Color::Accent()) : Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 13, 13);
        painter.drawPixmap(padding, 5, IconPixmap(icon_name, 16, foreground, devicePixelRatioF()));
        painter.setFont(font());
        painter.setPen(foreground);
        painter.drawText(QRect{padding + 22, 0, width() - padding - 22, height()},
                         Qt::AlignVCenter, text());
    }

    void enterEvent(QEnterEvent*) override {
        update();
    }
    void leaveEvent(QEvent*) override {
        update();
    }

private:
    QString icon_name;
    int padding;
    QColor ink = Color::Text();
    bool accent = false;
};

/// "60 → 120 FPS · 16.6 ms", painted so that a stats update only repaints.
class BarMetrics final : public QWidget {
public:
    explicit BarMetrics(QWidget* parent) : QWidget(parent) {
        setFixedHeight(26);
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
    }

    void Set(QString game, QString shown, QString time, bool at_target) {
        if (game == game_text && shown == shown_text && time == time_text &&
            at_target == on_target) {
            return;
        }
        game_text = std::move(game);
        shown_text = std::move(shown);
        time_text = std::move(time);
        on_target = at_target;
        const int width = Width();
        if (width != this->width()) {
            setFixedWidth(width);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        int x = 4;
        const auto span = [&](const QString& text, const QFont& font, const QColor& ink) {
            painter.setFont(font);
            painter.setPen(ink);
            const int advance = QFontMetrics(font).horizontalAdvance(text);
            painter.drawText(QRect{x, 0, advance + 1, height()}, Qt::AlignVCenter, text);
            x += advance + 5;
        };
        span(shown_text.isEmpty() ? InGameBar::tr("%1 FPS").arg(game_text) : game_text,
             Font(13, 700), on_target ? Color::Success() : Color::Text());
        if (!shown_text.isEmpty()) {
            span(QStringLiteral("→"), Font(12, 600), Color::TextSecondary());
            span(InGameBar::tr("%1 FPS").arg(shown_text), Font(13, 700), Color::Text());
        }
        span(QStringLiteral("·"), Font(12, 600), Color::TextSecondary());
        span(time_text, Font(13, 600), Color::TextSecondary());
    }

private:
    int Width() const {
        const QFontMetrics bold(Font(13, 700));
        const QFontMetrics small(Font(12, 600));
        int width = 8 + 5 + small.horizontalAdvance(QStringLiteral("·")) + 5 +
                    QFontMetrics(Font(13, 600)).horizontalAdvance(time_text);
        if (shown_text.isEmpty()) {
            width += bold.horizontalAdvance(InGameBar::tr("%1 FPS").arg(game_text));
        } else {
            width += bold.horizontalAdvance(game_text) + 5 +
                     small.horizontalAdvance(QStringLiteral("→")) + 5 +
                     bold.horizontalAdvance(InGameBar::tr("%1 FPS").arg(shown_text));
        }
        return width;
    }

    QString game_text;
    QString shown_text;
    QString time_text;
    bool on_target = false;
};

class MenuTooltip final : public QWidget {
public:
    explicit MenuTooltip(QWidget* parent)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint |
                              Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(250);
    }

    void SetKeys(QString keyboard_key, QString controller_key) {
        keyboard = std::move(keyboard_key);
        controller = std::move(controller_key);
        setFixedHeight(10 + 17 + (keyboard.isEmpty() ? 0 : 30) + (controller.isEmpty() ? 0 : 30) +
                       10);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Color::Menu());
        painter.drawRoundedRect(rect(), 12, 12);
        painter.setFont(Font(13, 600));
        painter.setPen(Color::Text());
        painter.drawText(QRect{12, 10, width() - 24, 17}, Qt::AlignVCenter,
                         InGameBar::tr("In-game menu"));
        int y = 33;
        const auto row = [&](const QString& name, const QString& key) {
            if (key.isEmpty()) {
                return;
            }
            painter.setFont(Font(12));
            painter.setPen(QColor(0xff, 0xff, 0xff, 0xbf));
            painter.drawText(QRect{12, y, 120, 24}, Qt::AlignVCenter, name);
            painter.setFont(Font(11, 600));
            const int key_width = painter.fontMetrics().horizontalAdvance(key) + 16;
            const QRectF cap{qreal(width() - 12 - key_width), qreal(y), qreal(key_width), 24};
            painter.setPen(QPen(QColor(0xff, 0xff, 0xff, 0x33), 1));
            painter.setBrush(Color::Control());
            painter.drawRoundedRect(cap.adjusted(0.5, 0.5, -0.5, -0.5), Radius::KeyCap,
                                    Radius::KeyCap);
            painter.setPen(Color::Text());
            painter.drawText(cap, Qt::AlignCenter, key);
            y += 30;
        };
        row(InGameBar::tr("Keyboard"), keyboard);
        row(InGameBar::tr("Controller"), controller);
    }

private:
    QString keyboard;
    QString controller;
};

InGameBar::InGameBar(QWidget* parent) : QStatusBar(parent), contents(new QWidget(this)) {
    setStyleSheet(QStringLiteral("QWidget { background: transparent; }"));
    setFixedHeight(36);
    setSizeGripEnabled(false);
    auto* layout = new QHBoxLayout(contents);
    layout->setContentsMargins(6, 1, 6, 0);
    layout->setSpacing(8);

    configuration = new ConfigurationButton(contents);
    configuration->setToolTip(tr("Quick controls"));
    layout->addWidget(configuration);
    connect(configuration, &QAbstractButton::clicked, this, &InGameBar::QuickControlsRequested);

    shaders = new ShaderChip(contents);
    layout->addWidget(shaders);

    speed = new BarChip({}, QStringLiteral("lightning"), 10, contents);
    speed->SetInk(Color::Info());
    speed->setToolTip(tr("Quick controls"));
    speed->hide();
    layout->addWidget(speed);
    connect(speed, &QAbstractButton::clicked, this, &InGameBar::QuickControlsRequested);

    notice = new QLabel(contents);
    notice->setFont(Font(12));
    notice->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    notice->setTextFormat(Qt::PlainText);
    SetTextColor(notice, Color::TextSecondary());
    layout->addWidget(notice, 1);
    connect(this, &QStatusBar::messageChanged, this, [this](const QString& message) {
        notice->setToolTip(message);
        notice->setText(notice->fontMetrics().elidedText(message, Qt::ElideRight, notice->width()));
    });

    metrics = new BarMetrics(contents);
    layout->addWidget(metrics);

    menu_button = new BarChip(tr("Menu"), QStringLiteral("sliders-horizontal"), 11, contents);
    menu_button->installEventFilter(this);
    menu_tooltip = new MenuTooltip(menu_button);
    connect(menu_button, &QAbstractButton::clicked, this, &InGameBar::MenuRequested);
    layout->addWidget(menu_button);
}

void InGameBar::SetConfiguration(const QString& renderer, const QString& mode) {
    configuration->setText(tr("%1 · %2").arg(renderer, mode));
    configuration->updateGeometry();
}

void InGameBar::SetStats(double fps, double generated, double frame_ms, bool at_target,
                         const QString& details) {
    const bool fg = std::isfinite(fps) && std::isfinite(generated) && generated >= 0.5;
    metrics->Set(std::isfinite(fps) ? QString::number(fps, 'f', 0) : QStringLiteral("—"),
                 fg ? QString::number(fps + generated, 'f', 0) : QString{},
                 std::isfinite(frame_ms) && frame_ms > 0 ? tr("%1 ms").arg(frame_ms, 0, 'f', 1)
                                                         : QStringLiteral("—"),
                 at_target);
    metrics->setToolTip(details);
}

void InGameBar::SetSpeedStatus(const QString& description) {
    if (speed->text() != description) {
        speed->setText(description);
        speed->updateGeometry();
    }
    speed->setVisible(!description.isEmpty());
}

void InGameBar::SetMenuShortcut(const QString& keyboard, const QString& controller) {
    menu_tooltip->SetKeys(keyboard, controller);
    menu_button->setToolTip(keyboard.isEmpty() ? tr("In-game menu")
                                               : tr("In-game menu (%1)").arg(keyboard));
}

void InGameBar::SetMenuOpen(bool open) {
    menu_tooltip->hide();
    menu_button->SetAccent(open);
}

void InGameBar::SetQuickControlsOpen(bool open) {
    configuration->setChecked(open);
}

QWidget* InGameBar::QuickControlsAnchor() const {
    return configuration;
}

void InGameBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), Color::Panel());
    painter.fillRect(QRect{0, 0, width(), 1}, QColor(0xff, 0xff, 0xff, 0x12));
}

void InGameBar::resizeEvent(QResizeEvent* event) {
    QStatusBar::resizeEvent(event);
    contents->setGeometry(rect());
    notice->setText(
        notice->fontMetrics().elidedText(currentMessage(), Qt::ElideRight, notice->width()));
}

bool InGameBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched != menu_button) {
        return QStatusBar::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::ToolTip: {
        const QPoint corner = menu_button->mapToGlobal(QPoint{menu_button->width(), 0});
        menu_tooltip->move(corner.x() - 14 - menu_tooltip->width(),
                           mapToGlobal(QPoint{}).y() - 10 - menu_tooltip->height());
        menu_tooltip->show();
        return true;
    }
    case QEvent::Leave:
    case QEvent::Hide:
    case QEvent::MouseButtonPress:
        menu_tooltip->hide();
        break;
    default:
        break;
    }
    return QStatusBar::eventFilter(watched, event);
}

} // namespace Spotlight
