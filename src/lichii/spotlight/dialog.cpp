// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "lichii/spotlight/dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <QWindow>

#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"

namespace Spotlight {

namespace {

constexpr char ChromeProperty[] = "spotlightChrome";
constexpr char SubtitleProperty[] = "spotlightSubtitle";
constexpr char TitleHiddenProperty[] = "spotlightTitleHidden";
constexpr char IconProperty[] = "spotlightIcon";
constexpr char IconTintProperty[] = "spotlightIconTint";
constexpr char ToneProperty[] = "spotlightTone";

// Room around the card for its shadow (Paper: 0 30 80 #000000a6), which falls down.
constexpr QMargins ShadowMargins{36, 20, 36, 52};
constexpr int ShadowBlur = 28;
constexpr int ShadowOffset = 18;
constexpr int Padding = 24;
constexpr int HeaderGap = 20;
constexpr int HeaderIconSize = 44;
constexpr int HeaderIconGap = 14;
constexpr int SubtitleGap = 4;
constexpr int CloseSize = 32;
constexpr int ButtonGap = 10;

// Message boxes: a 412 px card with the icon beside the title and text.
constexpr int MessageIconGap = 16;
constexpr int MessageTextWidth = 304;
constexpr int MessageWideTextWidth = 456;
constexpr int MessageTitleGap = 6;
constexpr int MessageButtonGap = 18;

struct MessageLook {
    QString icon;
    QColor color;
    int fill_alpha = 0;
};

MessageLook LookOf(const QMessageBox* box) {
    if (box->property(ToneProperty).toString() == QStringLiteral("success")) {
        return {QStringLiteral("check-circle"), Color::Success(), 0x1f};
    }
    switch (box->icon()) {
    case QMessageBox::Information:
        return {QStringLiteral("info"), Color::Info(), 0x1f};
    case QMessageBox::Warning:
        return {QStringLiteral("warning-circle"), Color::Warning(), 0x1f};
    case QMessageBox::Critical:
        return {QStringLiteral("warning-circle"), Color::Destructive(), 0x1f};
    case QMessageBox::Question:
        return {QStringLiteral("question"), Color::Accent(), 0x24};
    case QMessageBox::NoIcon:
        break;
    }
    return {};
}

QPixmap MessageIcon(const MessageLook& look, qreal ratio) {
    QPixmap tile(qRound(HeaderIconSize * ratio), qRound(HeaderIconSize * ratio));
    tile.setDevicePixelRatio(ratio);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    QColor fill = look.color;
    fill.setAlpha(look.fill_alpha);
    painter.setBrush(fill);
    painter.drawEllipse(QRectF(0, 0, HeaderIconSize, HeaderIconSize));
    painter.drawPixmap(11, 11, IconPixmap(look.icon, 22, look.color, ratio));
    return tile;
}

QPixmap HeaderIcon(const QString& name, const QColor& tint, qreal ratio) {
    QPixmap tile(qRound(HeaderIconSize * ratio), qRound(HeaderIconSize * ratio));
    tile.setDevicePixelRatio(ratio);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Color::Fill());
    painter.drawRoundedRect(QRectF(0, 0, HeaderIconSize, HeaderIconSize), Radius::Tile,
                            Radius::Tile);
    painter.drawPixmap(11, 11, IconPixmap(name, 22, tint, ratio));
    return tile;
}

// Dims the window under a modal dialog, as Paper draws every dialog.
class Scrim final : public QWidget {
public:
    explicit Scrim(QWidget* window) : QWidget(window) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(window->rect());
        window->installEventFilter(this);
        raise();
        show();
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
        }
        return false;
    }

    void paintEvent(QPaintEvent*) override {
        QPainter(this).fillRect(rect(), Color::DialogScrim());
    }
};

class DialogChrome final : public QObject {
public:
    DialogChrome(QDialog* dialog_, bool close_button)
        : QObject(dialog_), dialog(dialog_), box(qobject_cast<QMessageBox*>(dialog_)) {
        title = new QLabel(dialog);
        title->setObjectName(QStringLiteral("spotlightDialogTitle"));
        title->setFont(DisplayFont(box ? 20 : 22));
        title->setTextFormat(Qt::PlainText);
        title->setWordWrap(box != nullptr);
        SetTextColor(title, Color::Text());
        subtitle = new QLabel(dialog);
        subtitle->setObjectName(QStringLiteral("spotlightDialogSubtitle"));
        subtitle->setFont(Font(14, 400));
        subtitle->setTextFormat(Qt::PlainText);
        subtitle->setWordWrap(true);
        SetTextColor(subtitle, Color::TextSecondary());
        icon = new QLabel(dialog);
        icon->setFixedSize(HeaderIconSize, HeaderIconSize);
        icon->hide();
        if (close_button) {
            close = new CircleButton(QStringLiteral("x"), CloseSize, dialog);
            close->setToolTip(QDialog::tr("Close"));
            close->setFocusPolicy(Qt::NoFocus);
            connect(close, &QAbstractButton::clicked, dialog, &QDialog::reject);
        }
        dialog->installEventFilter(this);
        Update();
    }

    ~DialogChrome() override {
        delete scrim;
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        switch (event->type()) {
        case QEvent::WindowTitleChange:
        case QEvent::DynamicPropertyChange:
            Update();
            return false;
        case QEvent::Show:
            if (box) {
                PrepareMessageBox();
            }
            PrepareButtons();
            Update();
            ShowScrim();
            if (!shown) {
                shown = true;
                MakeRoomForChrome();
            }
            return false;
        case QEvent::Hide:
            delete scrim;
            return false;
        case QEvent::Resize:
            if (box) {
                FitMessageBox();
            }
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

    QLabel* MessageLabel() const {
        return dialog->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"),
                                          Qt::FindDirectChildrenOnly);
    }

    int TextLeft() const {
        return Card().left() + Padding + (icon->isHidden() ? 0 : HeaderIconSize + HeaderIconGap);
    }

    int TextRight() const {
        return Card().right() + 1 - Padding - (close ? CloseSize + HeaderIconGap : 0);
    }

    int HeaderHeight() const {
        const int width = std::max(1, TextRight() - TextLeft());
        int height = title->isHidden() ? 0 : title->sizeHint().height();
        if (!subtitle->isHidden()) {
            height += SubtitleGap + subtitle->heightForWidth(width);
        }
        if (!icon->isHidden()) {
            height = std::max(height, HeaderIconSize);
        }
        if (close) {
            height = std::max(height, CloseSize);
        }
        return height;
    }

    void Update() {
        title->setText(dialog->windowTitle());
        if (box) {
            title->setHidden(title->text().isEmpty());
            subtitle->hide();
            dialog->setContentsMargins(ShadowMargins +
                                       QMargins(Padding, Padding, Padding, Padding));
            Place();
            return;
        }
        title->setHidden(title->text().isEmpty() || dialog->property(TitleHiddenProperty).toBool());
        subtitle->setText(dialog->property(SubtitleProperty).toString());
        subtitle->setHidden(subtitle->text().isEmpty());
        const QString icon_name = dialog->property(IconProperty).toString();
        icon->setHidden(icon_name.isEmpty());
        if (!icon_name.isEmpty()) {
            icon->setPixmap(HeaderIcon(icon_name,
                                       dialog->property(IconTintProperty).value<QColor>(),
                                       dialog->devicePixelRatioF()));
        }
        const int header = HeaderHeight();
        dialog->setContentsMargins(
            ShadowMargins.left() + Padding,
            ShadowMargins.top() + Padding + header + (header ? HeaderGap : 0),
            ShadowMargins.right() + Padding, ShadowMargins.bottom() + Padding);
        Place();
    }

    void Place() {
        if (box) {
            PlaceMessageTitle();
            return;
        }
        const QRect card = Card();
        const int top = card.top() + Padding;
        const int left = TextLeft();
        const int width = std::max(1, TextRight() - left);
        icon->move(card.left() + Padding, top);
        title->setText(
            title->fontMetrics().elidedText(dialog->windowTitle(), Qt::ElideRight, width));
        int y = top;
        if (!icon->isHidden() && subtitle->isHidden()) {
            y += (HeaderIconSize - title->sizeHint().height()) / 2;
        }
        if (!title->isHidden()) {
            title->setGeometry(left, y, width, title->sizeHint().height());
            y += title->sizeHint().height() + SubtitleGap;
        }
        subtitle->setGeometry(left, y, width, subtitle->heightForWidth(width));
        if (close) {
            close->move(card.right() + 1 - Padding - CloseSize, top);
        }
    }

    void PlaceMessageTitle() {
        const QLabel* label = MessageLabel();
        if (!label || title->isHidden()) {
            return;
        }
        const QRect text = label->geometry();
        title->setGeometry(text.left(), text.top(), text.width(),
                           title->heightForWidth(text.width()));
    }

    void Paint() {
        QPainter painter(dialog);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(Card()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(Qt::NoPen);
        QColor shadow = Color::Shadow();
        for (int step = ShadowBlur; step > 0; --step) {
            const qreal t = 1.0 - qreal(step) / ShadowBlur;
            shadow.setAlphaF(0.05f * float(t * t));
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

    void ShowScrim() {
        QWidget* parent = dialog->parentWidget();
        if (scrim || !parent || !dialog->isModal()) {
            return;
        }
        QWidget* window = parent->window();
        if (window->property(ChromeProperty).toBool()) {
            return;
        }
        scrim = new Scrim(window);
    }

    // setupUi sizes a dialog from its form, which was drawn without the card around it, so the
    // dialog grows to the size its layout asks for when the form is too small.
    void MakeRoomForChrome() {
        if (box) {
            return;
        }
        QSize size = dialog->size();
        if (dialog->testAttribute(Qt::WA_Resized)) {
            const QMargins margins = dialog->contentsMargins();
            const QSize minimum = dialog->minimumSize();
            if (size.width() > minimum.width()) {
                size.rwidth() += margins.left() + margins.right();
            }
            if (size.height() > minimum.height()) {
                size.rheight() += margins.top() + margins.bottom();
            }
        }
        size = size.expandedTo(dialog->sizeHint()).boundedTo(dialog->maximumSize());
        // sizeHint ignores word-wrapped labels, which need more height at the final width.
        if (dialog->hasHeightForWidth()) {
            size.setHeight(std::max(size.height(), dialog->heightForWidth(size.width())));
        }
        if (const QScreen* screen = dialog->screen()) {
            size = size.boundedTo(screen->availableGeometry().size());
        }
        dialog->resize(size.expandedTo(dialog->minimumSizeHint()));
    }

    // Runs before QMessageBox::showEvent, which sizes the box from its layout.
    void PrepareMessageBox() {
        const MessageLook look = LookOf(box);
        if (!look.icon.isEmpty()) {
            box->setIconPixmap(MessageIcon(look, box->devicePixelRatioF()));
        }
        ApplyMessageSpacing();
    }

    void ApplyMessageSpacing() {
        if (auto* grid = qobject_cast<QGridLayout*>(box->layout())) {
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setHorizontalSpacing(0);
            grid->setVerticalSpacing(0);
        }
        if (auto* icon_label = box->findChild<QLabel*>(QStringLiteral("qt_msgboxex_icon_label"))) {
            icon_label->setContentsMargins(0, 0, MessageIconGap - 7, 0);
        }
        if (auto* informative =
                box->findChild<QLabel*>(QStringLiteral("qt_msgbox_informativelabel"))) {
            informative->setContentsMargins(0, MessageTitleGap, 0, 0);
        }
        if (auto* buttons = box->findChild<QDialogButtonBox*>()) {
            buttons->setContentsMargins(0, MessageButtonGap, 0, 0);
        }
        if (QCheckBox* check = box->checkBox()) {
            check->setContentsMargins(0, 12, 0, 0);
        }
    }

    // QMessageBox fits its text to the screen; Paper's message card wraps it at a fixed width
    // with the title above it, so the box is refitted each time QMessageBox sizes it.
    void FitMessageBox() {
        QLabel* label = MessageLabel();
        if (fitting || !label || !box->layout()) {
            return;
        }
        fitting = true;
        ApplyMessageSpacing();
        auto* informative = box->findChild<QLabel*>(QStringLiteral("qt_msgbox_informativelabel"));
        label->setWordWrap(true);
        int width = MessageTextWidth;
        if (label->heightForWidth(width) > 8 * label->fontMetrics().lineSpacing()) {
            width = MessageWideTextWidth;
        }
        const int title_height = title->isHidden() ? 0 : title->heightForWidth(width);
        label->setContentsMargins(0, title_height ? title_height + MessageTitleGap : 0, 0, 0);
        label->setFixedWidth(width);
        if (informative) {
            informative->setWordWrap(true);
            informative->setFixedWidth(width);
        }
        QLayout* layout = box->layout();
        layout->invalidate();
        layout->activate();
        const int total_width = layout->totalMinimumSize().width();
        const int height = layout->hasHeightForWidth() ? layout->totalHeightForWidth(total_width)
                                                       : layout->totalMinimumSize().height();
        box->setFixedSize(total_width, height);
        fitting = false;
    }

    void PrepareButtons() {
        for (auto* buttons : dialog->findChildren<QDialogButtonBox*>()) {
            if (buttons->layout()) {
                buttons->layout()->setSpacing(ButtonGap);
            }
            for (auto* button : buttons->buttons()) {
                if (buttons->buttonRole(button) == QDialogButtonBox::DestructiveRole &&
                    button->property("kind").isNull()) {
                    button->setProperty("kind", QStringLiteral("destructive"));
                    button->style()->unpolish(button);
                    button->style()->polish(button);
                }
            }
        }
        // Qt moves the default to whichever auto-default button has focus, and the accent with it;
        // Paper keeps the accent on the dialog's default button, the first accept button unless
        // one is set.
        const auto pushes = dialog->findChildren<QPushButton*>();
        QPushButton* chosen = nullptr;
        for (auto* push : pushes) {
            if (push->isDefault()) {
                chosen = push;
            }
        }
        for (auto* buttons : dialog->findChildren<QDialogButtonBox*>()) {
            for (auto* button : buttons->buttons()) {
                const auto role = buttons->buttonRole(button);
                if (!chosen &&
                    (role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole)) {
                    chosen = qobject_cast<QPushButton*>(button);
                }
            }
        }
        if (!chosen) {
            return;
        }
        // A button made default before it joined the dialog is unknown to QDialog, which then
        // loses the default when focus leaves it.
        chosen->setDefault(false);
        chosen->setDefault(true);
        for (auto* push : pushes) {
            push->setAutoDefault(push == chosen);
        }
    }

    QDialog* dialog;
    QMessageBox* box;
    QLabel* title;
    QLabel* subtitle;
    QLabel* icon;
    CircleButton* close = nullptr;
    QPointer<Scrim> scrim;
    bool shown = false;
    bool fitting = false;
};

} // namespace

Dialog::Dialog(QWidget* parent, Qt::WindowFlags flags) : QDialog(parent, flags) {
    AdoptDialogChrome(this);
}

void Dialog::SetSubtitle(const QString& subtitle) {
    setProperty(SubtitleProperty, subtitle);
}

void Dialog::SetIcon(QStringView name, const QColor& tint) {
    setProperty(IconTintProperty, tint);
    setProperty(IconProperty, name.toString());
}

void Dialog::SetTitleVisible(bool visible) {
    setProperty(TitleHiddenProperty, !visible);
}

void MarkSuccess(QMessageBox* box) {
    box->setProperty(ToneProperty, QStringLiteral("success"));
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
