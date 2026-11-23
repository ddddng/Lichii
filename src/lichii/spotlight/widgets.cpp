// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/spotlight/widgets.h"

#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>

#include "lichii/spotlight/theme.h"

namespace Spotlight {

namespace {
void FillRounded(QPainter& painter, const QRectF& rect, qreal radius, const QColor& fill,
                 const QColor& stroke = {}) {
    painter.setPen(stroke.isValid() ? QPen(stroke, 1) : Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(stroke.isValid() ? rect.adjusted(0.5, 0.5, -0.5, -0.5) : rect, radius, radius);
}

QColor Mix(const QColor& from, const QColor& to, qreal t) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                            from.greenF() + (to.greenF() - from.greenF()) * t,
                            from.blueF() + (to.blueF() - from.blueF()) * t,
                            from.alphaF() + (to.alphaF() - from.alphaF()) * t);
}

class ShowFade final : public QObject {
public:
    ShowFade(QWidget* window, int duration_) : QObject(window), duration(duration_) {
        window->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            auto* window = static_cast<QWidget*>(watched);
            window->setWindowOpacity(0);
            auto* animation = new QPropertyAnimation(window, "windowOpacity", window);
            animation->setDuration(duration);
            animation->setStartValue(0.0);
            animation->setEndValue(1.0);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            animation->start(QAbstractAnimation::DeleteWhenStopped);
        }
        return false;
    }

private:
    int duration;
};
} // namespace

void FadeIn(QWidget* widget, int duration) {
    if (!widget->isVisible() || widget->graphicsEffect() != nullptr) {
        return;
    }
    auto* effect = new QGraphicsOpacityEffect(widget);
    effect->setOpacity(0);
    widget->setGraphicsEffect(effect);
    auto* animation = new QPropertyAnimation(effect, "opacity", widget);
    animation->setDuration(duration);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(
        animation, &QAbstractAnimation::finished, widget,
        [widget, effect] {
            if (widget->graphicsEffect() == effect) {
                widget->setGraphicsEffect(nullptr);
            }
        },
        Qt::QueuedConnection);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void FadeInOnShow(QWidget* window, int duration) {
    new ShowFade(window, duration);
}

bool HasKeyboardFocus(const QWidget* widget) {
    return widget->hasFocus() && widget->window()->testAttribute(Qt::WA_KeyboardFocusChange);
}

HoverFade::HoverFade(QWidget* owner) {
    animation.setDuration(Motion::Hover);
    animation.setEasingCurve(QEasingCurve::OutCubic);
    animation.setStartValue(0.0);
    animation.setEndValue(0.0);
    QObject::connect(&animation, &QVariantAnimation::valueChanged, owner,
                     qOverload<>(&QWidget::update));
}

void HoverFade::SetHovered(bool hovered) {
    animation.stop();
    animation.setStartValue(Value());
    animation.setEndValue(hovered ? 1.0 : 0.0);
    animation.start();
}

qreal HoverFade::Value() const {
    return animation.currentValue().toReal();
}

CircleButton::CircleButton(QString icon, int diameter_, QWidget* parent)
    : QAbstractButton(parent), icon_name(std::move(icon)), diameter(diameter_) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setFixedSize(diameter, diameter);
}

void CircleButton::SetIcon(QString icon) {
    icon_name = std::move(icon);
    update();
}

QSize CircleButton::sizeHint() const {
    return {diameter, diameter};
}

void CircleButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor fill = isDown() || isChecked()
                            ? Color::Stroke()
                            : Mix(Color::Control(), Color::ControlHover(), hover.Value());
    FillRounded(painter, rect(), diameter / 2.0, fill,
                HasKeyboardFocus(this) ? Color::Accent() : Color::Stroke());
    const int icon = diameter >= 36 ? 20 : 16;
    const QPixmap pixmap = IconPixmap(icon_name, icon, isEnabled() ? Color::Text() : Color::TextFaint(),
                                      devicePixelRatioF());
    painter.drawPixmap((width() - icon) / 2, (height() - icon) / 2, pixmap);
}

void CircleButton::enterEvent(QEnterEvent*) {
    hover.SetHovered(true);
}

void CircleButton::leaveEvent(QEvent*) {
    hover.SetHovered(false);
}

PillButton::PillButton(const QString& text, QString icon, Kind kind_, int height_, QWidget* parent)
    : QAbstractButton(parent), icon_name(std::move(icon)), kind(kind_), height(height_) {
    setText(text);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setFont(Font(height >= 40 ? 15 : 13, 600));
    setFixedHeight(height);
}

void PillButton::SetKind(Kind kind_) {
    kind = kind_;
    update();
}

void PillButton::SetForegroundColor(const QColor& color) {
    foreground = color;
    update();
}

QSize PillButton::sizeHint() const {
    const int icon = icon_name.isEmpty() ? 0 : height >= 40 ? 26 : height == 26 ? 20 : 24;
    const int padding = height >= 40 ? 24 : height == 26 ? 11 : 16;
    return {fontMetrics().horizontalAdvance(text()) + icon + padding * 2, height};
}

void PillButton::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill, stroke, ink;
    switch (kind) {
    case Kind::Accent:
        fill = Mix(Color::Accent(), Color::AccentLight(), hover.Value());
        ink = Color::OnAccent();
        break;
    case Kind::Light:
        fill = Mix(Color::Text(), QColor(0xf2, 0xf3, 0xf7), hover.Value());
        ink = Color::OnLight();
        break;
    case Kind::Control:
        fill = Mix(height == 26 ? Color::Fill() : Color::Control(), Color::ControlHover(),
                   hover.Value());
        if (height != 26) {
            stroke = Color::Stroke();
        }
        ink = Color::Text();
        break;
    }
    if (foreground.isValid()) {
        ink = foreground;
    }
    if (!isEnabled()) {
        fill.setAlphaF(fill.alphaF() * 0.4f);
        ink.setAlphaF(0.45f);
    }
    if (HasKeyboardFocus(this)) {
        stroke = Color::Text();
    }
    FillRounded(painter, rect(), height / 2.0, fill, stroke);

    const int icon = icon_name.isEmpty() ? 0 : height >= 40 ? 18 : height == 26 ? 14 : 16;
    const int gap = icon ? (height == 26 ? 6 : 8) : 0;
    const int content = icon + gap + fontMetrics().horizontalAdvance(text());
    int x = (width() - content) / 2;
    if (icon) {
        painter.drawPixmap(x, (height - icon) / 2, IconPixmap(icon_name, icon, ink, devicePixelRatioF()));
        x += icon + gap;
    }
    painter.setPen(ink);
    painter.setFont(font());
    painter.drawText(QRect(x, 0, width() - x, height), Qt::AlignVCenter | Qt::AlignLeft, text());
}

void PillButton::enterEvent(QEnterEvent*) {
    hover.SetHovered(true);
}

void PillButton::leaveEvent(QEvent*) {
    hover.SetHovered(false);
}

SegmentedControl::SegmentedControl(QStringList options_, Kind kind_, int height_, QWidget* parent)
    : QWidget(parent), kind(kind_), height(height_) {
    setFocusPolicy(Qt::TabFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(height);
    slide.setDuration(Motion::Slide);
    slide.setEasingCurve(QEasingCurve::OutCubic);
    connect(&slide, &QVariantAnimation::valueChanged, this, qOverload<>(&QWidget::update));
    SetOptions(std::move(options_));
}

void SegmentedControl::SetOptions(QStringList options_) {
    options = std::move(options_);
    enabled = QList<bool>(options.size(), true);
    current = qBound(0, current, qMax(0, int(options.size()) - 1));
    setFont(Font(height >= 40 ? 14 : 13, 500));
    updateGeometry();
    update();
}

void SegmentedControl::SetCurrentIndex(int index) {
    if (index < 0 || index >= options.size() || index == current) {
        return;
    }
    const QRectF from = SelectionRect();
    current = index;
    slide.stop();
    if (isVisible()) {
        slide.setStartValue(from);
        slide.setEndValue(QRectF(SegmentRect(current)));
        slide.start();
    }
    update();
    emit CurrentChanged(index);
}

void SegmentedControl::SetOptionEnabled(int index, bool on) {
    if (index >= 0 && index < enabled.size()) {
        enabled[index] = on;
        update();
    }
}

QSize SegmentedControl::sizeHint() const {
    int width = 8 + 2 * qMax(0, int(options.size()) - 1);
    for (const auto& option : options) {
        width += QFontMetrics(Font(font().pixelSize(), 600)).horizontalAdvance(option) + 32;
    }
    return {width, height};
}

QRect SegmentedControl::SegmentRect(int index) const {
    int x = 4;
    for (int i = 0; i < options.size(); ++i) {
        const int w = QFontMetrics(Font(font().pixelSize(), 600)).horizontalAdvance(options[i]) + 32;
        if (i == index) {
            return {x, 4, w, height - 8};
        }
        x += w + 2;
    }
    return {};
}

QRectF SegmentedControl::SelectionRect() const {
    return slide.state() == QAbstractAnimation::Running ? slide.currentValue().toRectF()
                                                        : QRectF(SegmentRect(current));
}

int SegmentedControl::SegmentAt(const QPoint& point) const {
    for (int i = 0; i < options.size(); ++i) {
        if (SegmentRect(i).contains(point)) {
            return i;
        }
    }
    return -1;
}

void SegmentedControl::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect frame(0, 0, sizeHint().width(), height);
    FillRounded(painter, frame, height / 2.0, kind == Kind::Light ? Color::Control() : Color::Fill(),
                kind == Kind::Light ? Color::Stroke() : QColor{});
    if (!options.isEmpty()) {
        const QRectF selection = SelectionRect();
        FillRounded(painter, selection, selection.height() / 2.0,
                    kind == Kind::Light ? Color::Text() : Color::Accent());
    }
    for (int i = 0; i < options.size(); ++i) {
        const QRect segment = SegmentRect(i);
        const bool selected = i == current;
        QColor ink = Color::Text();
        if (selected) {
            ink = kind == Kind::Light ? Color::OnLight() : Color::OnAccent();
        } else if (i == hovered && enabled[i]) {
            FillRounded(painter, segment, segment.height() / 2.0, Color::Fill());
        }
        if (!enabled[i]) {
            ink = Color::TextFaint();
        }
        if (hasFocus() && selected) {
            painter.setPen(QPen(Color::Text(), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(segment).adjusted(-1.5, -1.5, 1.5, 1.5),
                                    segment.height() / 2.0 + 1.5, segment.height() / 2.0 + 1.5);
        }
        painter.setPen(ink);
        painter.setFont(Font(font().pixelSize(), selected ? 600 : 500));
        painter.drawText(segment, Qt::AlignCenter, options[i]);
    }
}

void SegmentedControl::mousePressEvent(QMouseEvent* event) {
    const int index = SegmentAt(event->position().toPoint());
    if (index >= 0 && enabled[index]) {
        SetCurrentIndex(index);
    }
}

void SegmentedControl::mouseMoveEvent(QMouseEvent* event) {
    const int index = SegmentAt(event->position().toPoint());
    if (index != hovered) {
        hovered = index;
        update();
    }
}

void SegmentedControl::leaveEvent(QEvent*) {
    hovered = -1;
    update();
}

void SegmentedControl::keyPressEvent(QKeyEvent* event) {
    const int step = event->key() == Qt::Key_Left ? -1 : event->key() == Qt::Key_Right ? 1 : 0;
    if (step == 0) {
        QWidget::keyPressEvent(event);
        return;
    }
    for (int i = current + step; i >= 0 && i < options.size(); i += step) {
        if (enabled[i]) {
            SetCurrentIndex(i);
            break;
        }
    }
}

ToggleSwitch::ToggleSwitch(QWidget* parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setFixedSize(sizeHint());
    knob.setDuration(Motion::Slide);
    knob.setEasingCurve(QEasingCurve::OutCubic);
    connect(&knob, &QVariantAnimation::valueChanged, this, qOverload<>(&QWidget::update));
}

void ToggleSwitch::nextCheckState() {
    knob.stop();
    knob.setStartValue(isChecked() ? 1.0 : 0.0);
    knob.setEndValue(isChecked() ? 0.0 : 1.0);
    QAbstractButton::nextCheckState();
    knob.start();
}

QSize ToggleSwitch::sizeHint() const {
    return {44, 26};
}

void ToggleSwitch::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal on = knob.state() == QAbstractAnimation::Running ? knob.currentValue().toReal()
                     : isChecked()                                ? 1.0
                                                                  : 0.0;
    QColor track = Mix(Color::ControlHover(), Color::Accent(), on);
    if (!isEnabled()) {
        track.setAlphaF(track.alphaF() * 0.4f);
    }
    FillRounded(painter, rect(), height() / 2.0, track, hasFocus() ? Color::Text() : QColor{});
    const int diameter = height() - 6;
    const qreal x = 3 + (width() - diameter - 6) * on;
    painter.setPen(Qt::NoPen);
    painter.setBrush(isEnabled() ? Color::Text() : Color::TextMuted());
    painter.drawEllipse(QRectF(x, 3, diameter, diameter));
}

KeyCap::KeyCap(QString text_, QWidget* parent) : QWidget(parent), text(std::move(text_)) {
    setFont(Font(11, 600));
    setFixedSize(sizeHint());
}

void KeyCap::SetText(QString text_) {
    text = std::move(text_);
    setFixedSize(sizeHint());
    update();
}

QSize KeyCap::sizeHint() const {
    return {fontMetrics().horizontalAdvance(text) + 16, 24};
}

void KeyCap::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    FillRounded(painter, rect(), Radius::KeyCap, Color::Control(), Color::Stroke());
    painter.setPen(Color::Text());
    painter.setFont(font());
    painter.drawText(rect(), Qt::AlignCenter, text);
}

} // namespace Spotlight
