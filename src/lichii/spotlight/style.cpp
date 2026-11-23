// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "lichii/spotlight/style.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QFocusEvent>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QProxyStyle>
#include <QScreen>
#include <QStyleFactory>

#include "lichii/spotlight/dialog.h"

namespace Spotlight {

namespace {

constexpr char KeyboardFocusProperty[] = "keyboardFocus";

bool IsPopupWindow(const QWidget* widget) {
    return widget->isWindow() &&
           (widget->inherits("QTipLabel") || widget->inherits("QComboBoxPrivateContainer") ||
            qobject_cast<const QAbstractItemView*>(widget) != nullptr);
}

// Qt sizes a combo box list to the box's own text width, without the list's frame, padding and
// scroll bar, which would elide the longest item.
void FitComboPopup(QWidget* container) {
    auto* view = container->findChild<QAbstractItemView*>();
    if (!view) {
        return;
    }
    const int missing = view->sizeHintForColumn(0) - view->viewport()->width();
    if (missing <= 0) {
        return;
    }
    QRect geometry = container->geometry();
    geometry.setWidth(geometry.width() + missing);
    if (const QScreen* screen = container->screen()) {
        const QRect available = screen->availableGeometry();
        geometry.moveRight(std::min(geometry.right(), available.right()));
        geometry.moveLeft(std::max(geometry.left(), available.left()));
    }
    container->setGeometry(geometry);
}

class Style final : public QProxyStyle {
public:
    Style() : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* data) const override {
        switch (hint) {
        case SH_ComboBox_Popup:
        case SH_DialogButtonBox_ButtonsHaveIcons:
        case SH_MessageBox_CenterButtons:
        case SH_ToolTip_Mask:
        case SH_Menu_Mask:
            return 0;
        default:
            return QProxyStyle::styleHint(hint, option, widget, data);
        }
    }

    void polish(QWidget* widget) override {
        QProxyStyle::polish(widget);
        if (!widget->testAttribute(Qt::WA_WState_Created) && IsPopupWindow(widget)) {
            widget->setAttribute(Qt::WA_TranslucentBackground);
        }
    }

    using QProxyStyle::polish;
};

// Gives dialogs their chrome while they are built, before their window exists and before the
// style sheet matches them (Qt creates a window before polishing it), and marks buttons that
// took focus from the keyboard, so their focus ring stays hidden after a click.
class AppFilter final : public QObject {
public:
    using QObject::QObject;
    bool active = false;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (!active) {
            return false;
        }
        switch (event->type()) {
        case QEvent::ChildAdded:
            if (auto* dialog = qobject_cast<QDialog*>(watched);
                dialog &&
                (qobject_cast<QMessageBox*>(dialog) || qobject_cast<QInputDialog*>(dialog) ||
                 qobject_cast<QProgressDialog*>(dialog))) {
                AdoptDialogChrome(dialog, false);
            }
            return false;
        case QEvent::Show:
            if (watched->inherits("QComboBoxPrivateContainer")) {
                FitComboPopup(static_cast<QWidget*>(watched));
            }
            return false;
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            if (auto* button = qobject_cast<QAbstractButton*>(watched)) {
                const auto reason = static_cast<QFocusEvent*>(event)->reason();
                const bool keyboard =
                    event->type() == QEvent::FocusIn &&
                    (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason ||
                     reason == Qt::ShortcutFocusReason);
                if (button->property(KeyboardFocusProperty).toBool() != keyboard) {
                    button->setProperty(KeyboardFocusProperty, keyboard);
                    button->style()->unpolish(button);
                    button->style()->polish(button);
                }
            }
            return false;
        default:
            return false;
        }
    }
};

} // namespace

void ApplyAppStyle(bool spotlight) {
    static const QString platform_style = QApplication::style()->name();
    static QPointer<AppFilter> filter;
    static bool applied = false;
    if (!filter) {
        filter = new AppFilter(qApp);
        qApp->installEventFilter(filter);
    }
    filter->active = spotlight;
    if (spotlight == applied) {
        return;
    }
    applied = spotlight;
    if (spotlight) {
        QApplication::setStyle(new Style);
    } else {
        QApplication::setStyle(platform_style);
    }
}

} // namespace Spotlight
