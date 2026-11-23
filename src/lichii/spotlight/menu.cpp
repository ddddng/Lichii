// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include <QApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QStyleOptionMenuItem>

#include "lichii/spotlight/menu.h"
#include "lichii/spotlight/theme.h"
#include "qt_common/game_list/game_list_p.h"

namespace Spotlight {

namespace {

constexpr char PanelIcon[] = "spotlightIcon";

QString HintOf(const QAction* action) {
    const QString hint = action->text().section(QLatin1Char('\t'), 1);
    if (!hint.isEmpty()) {
        return hint;
    }
    return action->shortcut()
        .toString(QKeySequence::NativeText)
        .replace(QLatin1Char('+'), QLatin1Char(' '));
}

int TrailingWidth(const QAction* action, const QFontMetrics& hint_metrics) {
    if (action->menu()) {
        return 32;
    }
    const QString hint = HintOf(action);
    return hint.isEmpty() ? 12 : 22 + hint_metrics.horizontalAdvance(hint);
}

int ContentWidth(const QMenu& menu) {
    const QFontMetrics label_metrics(Font(14));
    const QFontMetrics hint_metrics(Font(12));
    int width = 0;
    for (const QAction* action : menu.actions()) {
        if (!action->isVisible() || action->isSeparator()) {
            continue;
        }
        const QString label =
            action->text().section(QLatin1Char('\t'), 0, 0).remove(QLatin1Char('&'));
        width = std::max(width, 43 + label_metrics.horizontalAdvance(label) +
                                    TrailingWidth(action, hint_metrics));
    }
    return width + 16;
}

void PaintMenu(QMenu& menu, const QPaintEvent& event) {
    QPainter painter(&menu);
    painter.setRenderHint(QPainter::Antialiasing);
    QStyleOption panel;
    panel.initFrom(&menu);
    menu.style()->drawPrimitive(QStyle::PE_PanelMenu, &panel, &painter, &menu);
    const auto ratio = menu.devicePixelRatioF();
    const auto direction = menu.layoutDirection();
    const auto align = (direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft) |
                       Qt::AlignVCenter;
    const QFontMetrics hint_metrics(Font(12));
    for (auto* action : menu.actions()) {
        const QRect row = menu.actionGeometry(action);
        if (!action->isVisible() || row.isEmpty() || !event.rect().intersects(row)) {
            continue;
        }
        if (action->isSeparator()) {
            QStyleOptionMenuItem item;
            item.initFrom(&menu);
            item.menuItemType = QStyleOptionMenuItem::Separator;
            item.rect = row;
            menu.style()->drawControl(QStyle::CE_MenuItem, &item, &painter, &menu);
            continue;
        }
        const bool enabled = action->isEnabled();
        if (action == menu.activeAction() && enabled) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Color::RowHover());
            painter.drawRoundedRect(row, 10, 10);
        }
        const auto visual = [&](const QRect& logical) {
            return QStyle::visualRect(direction, row, logical);
        };
        const QRect leading = visual({row.left() + 12, row.center().y() - 7, 16, 16});
        const auto ink = !enabled ? Color::TextFaint()
                         : action->property("spotlightDestructive").toBool() ? Color::AccentLight()
                                                                             : Color::MenuInk();
        const QIcon panel_icon = action->property(PanelIcon).value<QIcon>();
        const QIcon icon = panel_icon.isNull() ? action->icon() : panel_icon;
        if (action->isChecked()) {
            painter.drawPixmap(leading.topLeft(), IconPixmap(u"check", 16, Color::Accent(), ratio));
        } else if (!icon.isNull()) {
            painter.drawPixmap(leading.topLeft(),
                               icon.pixmap({16, 16}, ratio, enabled ? QIcon::Normal : QIcon::Disabled));
        }
        const QString hint = HintOf(action);
        if (action->menu()) {
            const QRect arrow = visual({row.right() - 27, row.center().y() - 7, 16, 16});
            painter.drawPixmap(arrow.topLeft(),
                               IconPixmap(direction == Qt::RightToLeft ? u"caret-left"
                                                                       : u"caret-right",
                                          16, Color::TextMuted(), ratio));
        } else if (!hint.isEmpty()) {
            painter.setFont(Font(12));
            painter.setPen(Color::TextMuted());
            const int width = hint_metrics.horizontalAdvance(hint);
            painter.drawText(visual({row.right() - 11 - width, row.top(), width, row.height()}),
                             align, hint);
        }
        painter.setFont(Font(14));
        painter.setPen(ink);
        const QRect label = row.adjusted(39, 0, -TrailingWidth(action, hint_metrics), 0);
        painter.drawText(visual(label), align | Qt::TextHideMnemonic,
                         painter.fontMetrics().elidedText(
                             action->text().section(QLatin1Char('\t'), 0, 0), Qt::ElideRight,
                             label.width(), Qt::TextShowMnemonic));
    }
}

bool FitsPanel(const QMenu& menu) {
    const auto items = menu.actions();
    return std::none_of(items.cbegin(), items.cend(), [&menu](QAction* action) {
        const QRect row = menu.actionGeometry(action);
        return action->isVisible() && !row.isEmpty() && !menu.rect().contains(row);
    });
}

class MenuStyle final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* menu = qobject_cast<QMenu*>(watched);
        if (!menu || !IsLichiiTheme()) {
            return false;
        }
        switch (event->type()) {
        case QEvent::Polish:
            if (!menu->testAttribute(Qt::WA_WState_Created)) {
                menu->setAttribute(Qt::WA_TranslucentBackground);
                menu->setWindowFlag(Qt::NoDropShadowWindowHint);
            }
            if (!menu->property("spotlightSized").toBool()) {
                menu->setProperty("spotlightSized", true);
                connect(menu, &QMenu::aboutToShow, menu, [menu] {
                    menu->setMinimumWidth(std::max(menu->minimumWidth(), ContentWidth(*menu)));
                });
            }
            return false;
        case QEvent::Paint:
            if (!FitsPanel(*menu)) {
                return false;
            }
            PaintMenu(*menu, *static_cast<QPaintEvent*>(event));
            return true;
        default:
            return false;
        }
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
    addMenu(menu)->setProperty(PanelIcon, icon);
}

void InstallMenuStyle(QApplication& application) {
    application.installEventFilter(new MenuStyle(&application));
}

} // namespace Spotlight
