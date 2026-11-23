// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QStyleOptionMenuItem>

#include "lichii/spotlight/menu.h"
#include "lichii/spotlight/theme.h"
#include "qt_common/game_list/game_list_p.h"

namespace Spotlight {

namespace {

class MenuStyle final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Polish) {
            auto* menu = qobject_cast<QMenu*>(watched);
            if (menu && !menu->testAttribute(Qt::WA_WState_Created) && IsLichiiTheme()) {
                menu->setAttribute(Qt::WA_TranslucentBackground);
                menu->setWindowFlag(Qt::NoDropShadowWindowHint);
            }
        }
        return false;
    }
};

} // namespace

Menu::Menu(QWidget* parent) : QMenu(parent) {}

Menu* Menu::AddSubmenu(const QIcon& icon, const QString& title) {
    auto* menu = new Menu(this);
    menu->setTitle(title);
    menu->setIcon(icon);
    addMenu(menu);
    return menu;
}

void Menu::AddSharedMenu(QMenu* menu, const QIcon& icon) {
    shared_icons.insert(addMenu(menu), icon);
}

void Menu::paintEvent(QPaintEvent* event) {
    if (!IsLichiiTheme()) {
        QMenu::paintEvent(event);
        return;
    }
    const auto items = actions();
    for (auto* action : items) {
        const QRect row = actionGeometry(action);
        if (action->isVisible() && !row.isEmpty() && !rect().contains(row)) {
            QMenu::paintEvent(event);
            return;
        }
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QStyleOption panel;
    panel.initFrom(this);
    style()->drawPrimitive(QStyle::PE_PanelMenu, &panel, &painter, this);
    const auto ratio = devicePixelRatioF();
    const bool right_to_left = layoutDirection() == Qt::RightToLeft;
    const auto align = (right_to_left ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter;
    for (auto* action : items) {
        const QRect row = actionGeometry(action);
        if (!action->isVisible() || row.isEmpty() || !event->rect().intersects(row)) {
            continue;
        }
        if (action->isSeparator()) {
            QStyleOptionMenuItem item;
            initStyleOption(&item, action);
            item.rect = row;
            style()->drawControl(QStyle::CE_MenuItem, &item, &painter, this);
            continue;
        }
        const bool enabled = action->isEnabled();
        if (action == activeAction() && enabled) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Color::RowHover());
            painter.drawRoundedRect(row, 10, 10);
        }
        const auto visual = [&](const QRect& logical) {
            return QStyle::visualRect(layoutDirection(), row, logical);
        };
        const QRect leading = visual({row.left() + 12, row.center().y() - 7, 16, 16});
        const auto ink = !enabled ? Color::TextFaint()
                         : action->property("spotlightDestructive").toBool() ? Color::AccentLight()
                                                                             : Color::MenuInk();
        if (action->isChecked()) {
            painter.drawPixmap(leading.topLeft(), IconPixmap(u"check", 16, Color::Accent(), ratio));
        } else if (const QIcon icon = shared_icons.value(action, action->icon()); !icon.isNull()) {
            painter.drawPixmap(leading.topLeft(),
                               icon.pixmap({16, 16}, ratio, enabled ? QIcon::Normal : QIcon::Disabled));
        }
        const auto text = action->text().split(QLatin1Char('\t'));
        QString hint = text.value(1);
        if (hint.isEmpty()) {
            hint = action->shortcut()
                       .toString(QKeySequence::NativeText)
                       .replace(QLatin1Char('+'), QLatin1Char(' '));
        }
        int trailing = 12;
        if (action->menu()) {
            trailing += 20;
            const QRect arrow = visual({row.right() - 27, row.center().y() - 7, 16, 16});
            painter.drawPixmap(arrow.topLeft(),
                               IconPixmap(right_to_left ? u"caret-left" : u"caret-right", 16,
                                          Color::TextMuted(), ratio));
        } else if (!hint.isEmpty()) {
            painter.setFont(Font(12));
            painter.setPen(Color::TextMuted());
            const int width = painter.fontMetrics().horizontalAdvance(hint);
            painter.drawText(visual({row.right() - 11 - width, row.top(), width, row.height()}),
                             align, hint);
            trailing += width + 10;
        }
        painter.setFont(Font(14));
        painter.setPen(ink);
        const QRect label = row.adjusted(39, 0, -trailing, 0);
        painter.drawText(visual(label), align | Qt::TextHideMnemonic,
                         painter.fontMetrics().elidedText(text.first(), Qt::ElideRight,
                                                          label.width(), Qt::TextShowMnemonic));
    }
}

void InstallMenuStyle(QApplication& application) {
    application.installEventFilter(new MenuStyle(&application));
}

} // namespace Spotlight
