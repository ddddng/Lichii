// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "lichii/spotlight/dialog.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyle>
#include <QWindow>

#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace Spotlight {

namespace {

constexpr char ChromeProperty[] = "spotlightChrome";
constexpr char SubtitleProperty[] = "spotlightSubtitle";
constexpr char TitleHiddenProperty[] = "spotlightTitleHidden";

// Room around the card for its shadow, which falls 8 px down.
constexpr QMargins ShadowMargins{24, 16, 24, 32};
constexpr int ShadowBlur = 16;
constexpr int ShadowOffset = 8;
constexpr int PaddingX = 14;
constexpr int PaddingTop = 18;
constexpr int PaddingBottom = 16;
constexpr int TitleGap = 6;
constexpr int CloseSize = 30;

QPixmap MessageIcon(QMessageBox::Icon icon, qreal ratio) {
    QString name;
    QColor tint;
    switch (icon) {
    case QMessageBox::Information:
        name = QStringLiteral("info");
        tint = Color::Info();
        break;
    case QMessageBox::Warning:
        name = QStringLiteral("warning-circle");
        tint = Color::Warning();
        break;
    case QMessageBox::Critical:
        name = QStringLiteral("warning-circle");
        tint = Color::Destructive();
        break;
    case QMessageBox::Question:
        name = QStringLiteral("question");
        tint = Color::Accent();
        break;
    case QMessageBox::NoIcon:
        return {};
    }
    constexpr int size = 44;
    QPixmap tile(qRound(size * ratio), qRound(size * ratio));
    tile.setDevicePixelRatio(ratio);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    QColor fill = tint;
    fill.setAlpha(0x24);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(0, 0, size, size), 15, 15);
    painter.drawPixmap(11, 11, IconPixmap(name, 22, tint, ratio));
    return tile;
}

class DialogChrome final : public QObject {
public:
    DialogChrome(QDialog* dialog_, bool close_button) : QObject(dialog_), dialog(dialog_) {
        title = new QLabel(dialog);
        title->setObjectName(QStringLiteral("spotlightDialogTitle"));
        title->setFont(Font(18, 700));
        title->setTextFormat(Qt::PlainText);
        SetTextColor(title, Color::Text());
        subtitle = new QLabel(dialog);
        subtitle->setObjectName(QStringLiteral("spotlightDialogSubtitle"));
        subtitle->setFont(Font(12, 500));
        subtitle->setTextFormat(Qt::PlainText);
        SetTextColor(subtitle, Color::TextSecondary());
        if (close_button) {
            close = new CircleButton(QStringLiteral("x"), CloseSize, dialog);
            close->setToolTip(QDialog::tr("Close"));
            close->setFocusPolicy(Qt::NoFocus);
            connect(close, &QAbstractButton::clicked, dialog, &QDialog::reject);
        }
        dialog->installEventFilter(this);
        Update();
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        switch (event->type()) {
        case QEvent::WindowTitleChange:
        case QEvent::DynamicPropertyChange:
            Update();
            return false;
        case QEvent::Show:
            PrepareMessageBox();
            MarkDestructiveButtons();
            Update();
            if (!shown) {
                shown = true;
                MakeRoomForChrome();
            }
            return false;
        case QEvent::Resize:
            Place();
            return false;
        case QEvent::Paint:
            Paint();
            return false;
        case QEvent::MouseButtonPress:
            if (static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton &&
                dialog->windowHandle()) {
                dialog->windowHandle()->startSystemMove();
                return true;
            }
            return false;
        default:
            return false;
        }
    }

private:
    QRect Card() const {
        return dialog->rect().marginsRemoved(ShadowMargins);
    }

    int TitleHeight() const {
        int height = title->isHidden() ? 0 : title->sizeHint().height();
        if (!subtitle->isHidden()) {
            height += 2 + subtitle->sizeHint().height();
        }
        if (close) {
            height = std::max(height, CloseSize);
        }
        return height;
    }

    void Update() {
        title->setText(dialog->windowTitle());
        title->setHidden(title->text().isEmpty() || dialog->property(TitleHiddenProperty).toBool());
        subtitle->setText(dialog->property(SubtitleProperty).toString());
        subtitle->setHidden(subtitle->text().isEmpty());
        const int title_height = TitleHeight();
        dialog->setContentsMargins(
            ShadowMargins.left() + PaddingX,
            ShadowMargins.top() + PaddingTop + title_height + (title_height ? TitleGap : 0),
            ShadowMargins.right() + PaddingX, ShadowMargins.bottom() + PaddingBottom);
        Place();
    }

    void Place() {
        const QRect card = Card();
        const int left = card.left() + PaddingX + 8;
        const int right = card.right() - PaddingX - (close ? 4 : 8);
        const int top = card.top() + PaddingTop;
        const int text_right = close ? right - CloseSize - 8 : right;
        const int title_height = TitleHeight();
        const int text_height = title->sizeHint().height() +
                                (subtitle->isHidden() ? 0 : 2 + subtitle->sizeHint().height());
        int y = top + (title_height - text_height) / 2;
        title->setText(title->fontMetrics().elidedText(dialog->windowTitle(), Qt::ElideRight,
                                                       text_right - left));
        title->setGeometry(left, y, text_right - left, title->sizeHint().height());
        y += title->sizeHint().height() + 2;
        subtitle->setGeometry(left, y, text_right - left, subtitle->sizeHint().height());
        if (close) {
            close->move(right - CloseSize + 1, top + (title_height - CloseSize) / 2);
        }
    }

    void Paint() {
        QPainter painter(dialog);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(Card()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(Qt::NoPen);
        QColor shadow = Color::Shadow();
        for (int step = ShadowBlur; step > 0; --step) {
            const qreal t = 1.0 - qreal(step) / ShadowBlur;
            shadow.setAlphaF(0.06f * float(t * t));
            painter.setBrush(shadow);
            const qreal grow = step;
            painter.drawRoundedRect(
                card.translated(0, ShadowOffset * t).adjusted(-grow, -grow, grow, grow),
                Radius::Dialog + grow, Radius::Dialog + grow);
        }
        painter.setPen(QPen(Color::StrokeSubtle(), 1));
        painter.setBrush(Color::DialogSolid());
        painter.drawRoundedRect(card, Radius::Dialog, Radius::Dialog);
    }

    // setupUi sizes a dialog from its form, which was drawn without the card around it, so the
    // dialog grows to the size its layout asks for when the form is too small.
    void MakeRoomForChrome() {
        dialog->resize(dialog->size().expandedTo(dialog->sizeHint()));
    }

    void PrepareMessageBox() {
        auto* box = qobject_cast<QMessageBox*>(dialog);
        if (!box || box->icon() == QMessageBox::NoIcon) {
            return;
        }
        box->setIconPixmap(MessageIcon(box->icon(), box->devicePixelRatioF()));
    }

    void MarkDestructiveButtons() {
        for (auto* box : dialog->findChildren<QDialogButtonBox*>()) {
            for (auto* button : box->buttons()) {
                if (box->buttonRole(button) == QDialogButtonBox::DestructiveRole &&
                    button->property("kind").isNull()) {
                    button->setProperty("kind", QStringLiteral("destructive"));
                    button->style()->unpolish(button);
                    button->style()->polish(button);
                }
            }
        }
    }

    QDialog* dialog;
    QLabel* title;
    QLabel* subtitle;
    CircleButton* close = nullptr;
    bool shown = false;
};

} // namespace

Dialog::Dialog(QWidget* parent, Qt::WindowFlags flags) : QDialog(parent, flags) {
    AdoptDialogChrome(this);
}

void Dialog::SetSubtitle(const QString& subtitle) {
    setProperty(SubtitleProperty, subtitle);
}

void Dialog::SetTitleVisible(bool visible) {
    setProperty(TitleHiddenProperty, !visible);
}

void AdoptDialogChrome(QDialog* dialog, bool close_button) {
    if (dialog->property(ChromeProperty).toBool() || dialog->testAttribute(Qt::WA_WState_Created)) {
        return;
    }
    dialog->setProperty(ChromeProperty, true);
    dialog->setAttribute(Qt::WA_TranslucentBackground);
    dialog->overrideWindowFlags(dialog->windowFlags() | Qt::FramelessWindowHint);
    new DialogChrome(dialog, close_button);
}

} // namespace Spotlight
