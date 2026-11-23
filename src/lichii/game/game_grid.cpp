// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QMouseEvent>

#include "lichii/game/game_card.h"
#include "lichii/game/game_grid.h"
#include "qt_common/config/uisettings.h"

GameGrid::GameGrid(QWidget* parent, Mode mode_)
    : QListView(parent), m_gameCard(new GameCard(this)), mode(mode_) {
    setItemDelegate(m_gameCard);
    setViewMode(QListView::IconMode);
    setMovement(QListView::Static);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    if (mode == Mode::Grid) {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFlow(QListView::LeftToRight);
    setWrapping(mode == Mode::Grid);
    setViewportMargins(0, 0, 32, 0);
    setObjectName(mode == Mode::Grid ? QStringLiteral("spotlightGrid")
                                     : QStringLiteral("spotlightCarousel"));
    UpdateIconSize();
}

void GameGrid::UpdateIconSize() {
    setGridSize(m_gameCard->UpdateSize(mode == Mode::Carousel ? viewport()->height() : 0));
    viewport()->update();
}

QModelIndex GameGrid::indexAt(const QPoint& point) const {
    const QModelIndex index =
        QListView::indexAt(point - QPoint(GameCard::HitShift, GameCard::HitShift));
    return index.isValid() && m_gameCard->HitTest(point, visualRect(index)) ? index : QModelIndex{};
}

void GameGrid::setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) {
    const QModelIndex index = indexAt(rect.center());
    if (index.isValid()) {
        QListView::setSelection(QRect(visualRect(index).center(), QSize(1, 1)), command);
    }
}

void GameGrid::UpdateEffects(const QModelIndex& index) {
    if (index.isValid()) {
        constexpr int margin = GameCard::EffectMargin;
        viewport()->update(visualRect(index).adjusted(-margin, -margin, margin, margin));
    }
}

void GameGrid::dataChanged(const QModelIndex& top_left, const QModelIndex& bottom_right,
                           const QList<int>& roles) {
    QListView::dataChanged(top_left, bottom_right, roles);
    viewport()->update();
}

void GameGrid::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QListView::currentChanged(current, previous);
    UpdateEffects(previous);
    UpdateEffects(current);
}

bool GameGrid::viewportEvent(QEvent* event) {
    switch (event->type()) {
    case QEvent::ToolTip:
        return true;
    case QEvent::HoverEnter:
    case QEvent::HoverMove:
    case QEvent::HoverLeave:
    case QEvent::Leave: {
        const QModelIndex index =
            event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove
                ? indexAt(static_cast<QHoverEvent*>(event)->position().toPoint())
                : QModelIndex{};
        if (index != hovered) {
            UpdateEffects(hovered);
            UpdateEffects(index);
            hovered = index;
        }
        break;
    }
    default:
        break;
    }
    return QListView::viewportEvent(event);
}

void GameGrid::mousePressEvent(QMouseEvent* event) {
    const auto index = indexAt(event->pos());
    if (event->button() == Qt::LeftButton && index.isValid() &&
        UISettings::values.game_cover_size.GetValue() >= 64 &&
        m_gameCard->PlayRect(visualRect(index)).contains(event->pos())) {
        pending_play = index;
    }
    QListView::mousePressEvent(event);
}

void GameGrid::mouseReleaseEvent(QMouseEvent* event) {
    const QModelIndex index = pending_play;
    pending_play = QModelIndex{};
    QListView::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton && index.isValid() && index == indexAt(event->pos()) &&
        m_gameCard->PlayRect(visualRect(index)).contains(event->pos())) {
        emit PlayRequested(index);
    }
}
