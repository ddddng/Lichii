// SPDX-FileCopyrightText: Copyright 2026 Lichii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QLabel;

namespace Spotlight {

/// An inline status box under a form: a tinted icon and wrapped text in a rounded card, such as
/// "Testing iiDB access…" or "Check or replace your iiDB API key."
class Notice final : public QWidget {
    Q_OBJECT
public:
    enum class Tone { Neutral, Success, Error, Warning };

    explicit Notice(QWidget* parent = nullptr);

    void SetTone(Tone tone);
    void SetText(const QString& text);
    QString Text() const;
    /// Lets the text open links instead of showing plain text.
    void SetOpenExternalLinks(bool open);

protected:
    void paintEvent(QPaintEvent*) override;
    void changeEvent(QEvent* event) override;

private:
    void UpdateIcon();

    QLabel* icon;
    QLabel* label;
    Tone tone = Tone::Neutral;
};

} // namespace Spotlight
