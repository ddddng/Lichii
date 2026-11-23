// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lichii/nup_dialogs.h"

#include <QBuffer>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QScrollArea>
#include <QSlider>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "lichii/spotlight/theme.h"
#include "lichii/spotlight/widgets.h"
#include "lichii/util/icon_search.h"

namespace {

constexpr int IconSize = 256;
constexpr int HeaderIconSize = 96;
constexpr int LabelWidth = 96;

// Scoped to the card, so message boxes and file dialogs opened from a card keep their own style.
constexpr auto CardStyle = R"(
ArchiveCardDialog { background: transparent; }
QWidget#archiveCard, QWidget#archiveCard QWidget { background: transparent; }
QWidget#archiveCard QLineEdit {
    background-color: rgba(255, 255, 255, 13);
    border: 1px solid rgba(255, 255, 255, 36);
    border-radius: 12px;
    min-height: 34px;
    padding: 0 12px;
    font-family: "Lexend";
    font-size: 13px;
    color: #ffffff;
}
QWidget#archiveCard QLineEdit:hover { border-color: rgba(255, 255, 255, 61); }
QWidget#archiveCard QLineEdit:focus {
    border-color: #ff6b95;
    background-color: rgba(255, 255, 255, 20);
}
QWidget#archiveCard QLineEdit:disabled {
    color: rgba(255, 255, 255, 92);
    border-color: rgba(255, 255, 255, 20);
}
QWidget#archiveCard QProgressBar {
    background-color: rgba(255, 255, 255, 20);
    border: 0;
    border-radius: 4px;
    min-height: 8px;
    max-height: 8px;
}
QWidget#archiveCard QProgressBar::chunk {
    background-color: #ff6b95;
    border-radius: 4px;
}
)";

QString ToQString(const std::filesystem::path& path) {
    return QString::fromStdString(Common::FS::PathToUTF8String(path));
}

std::filesystem::path FromQString(const QString& path) {
    const auto utf8 = path.toStdString();
    return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
}

QString HumanSize(u64 bytes) {
    constexpr double GiB = 1024.0 * 1024 * 1024;
    if (bytes >= GiB) {
        return QObject::tr("%1 GB").arg(bytes / GiB, 0, 'f', 1);
    }
    if (bytes >= (u64{1} << 20)) {
        return QObject::tr("%1 MB").arg(bytes >> 20);
    }
    return QObject::tr("%1 KB").arg(std::max<u64>(1, bytes >> 10));
}

QPixmap PixmapFromBytes(const std::vector<u8>& bytes) {
    QPixmap pixmap;
    pixmap.loadFromData(bytes.data(), static_cast<uint>(bytes.size()));
    return pixmap;
}

QPixmap RoundedPreview(const QPixmap& source, int size) {
    const qreal ratio = 2.0;
    QPixmap out(QSize(size, size) * ratio);
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, 0, size, size), Spotlight::Radius::Tile,
                        Spotlight::Radius::Tile);
    painter.setClipPath(clip);
    painter.drawPixmap(QRectF(0, 0, size, size), source, source.rect());
    return out;
}

QImage ReadImage(const QString& path) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    return reader.read();
}

QRect CenterSquare(const QImage& image) {
    const int side = std::min(image.width(), image.height());
    return QRect((image.width() - side) / 2, (image.height() - side) / 2, side, side);
}

QLabel* TextLabel(const QString& text, int size, int weight, const QColor& color) {
    auto* label = new QLabel(text);
    label->setFont(Spotlight::Font(size, weight));
    Spotlight::SetTextColor(label, color);
    return label;
}

struct LocalizedLanguage {
    const char* tag;
    const char* name;
};

// The languages the console stores a name for; "en-us" is the Name field itself, and tags not
// listed here are kept as stored.
constexpr std::array<LocalizedLanguage, 15> LocalizedLanguages{{
    {"en-gb", QT_TRANSLATE_NOOP("ArchiveEditDialog", "English (UK)")},
    {"ja", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Japanese")},
    {"fr", QT_TRANSLATE_NOOP("ArchiveEditDialog", "French")},
    {"de", QT_TRANSLATE_NOOP("ArchiveEditDialog", "German")},
    {"es-419", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Spanish (Latin America)")},
    {"es", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Spanish")},
    {"it", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Italian")},
    {"nl", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Dutch")},
    {"fr-ca", QT_TRANSLATE_NOOP("ArchiveEditDialog", "French (Canada)")},
    {"pt", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Portuguese")},
    {"ru", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Russian")},
    {"ko", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Korean")},
    {"zh-hant", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Chinese (Traditional)")},
    {"zh-hans", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Chinese (Simplified)")},
    {"pt-br", QT_TRANSLATE_NOOP("ArchiveEditDialog", "Portuguese (Brazil)")},
}};

bool IsEditableLanguage(const std::string& tag) {
    return std::ranges::any_of(LocalizedLanguages,
                               [&](const auto& language) { return tag == language.tag; });
}

// The first dropped local file that Qt can read as an image.
QString DroppedImage(const QMimeData* mime) {
    if (mime == nullptr) {
        return {};
    }
    for (const auto& url : mime->urls()) {
        if (url.isLocalFile() && !QImageReader(url.toLocalFile()).format().isEmpty()) {
            return url.toLocalFile();
        }
    }
    return {};
}

// A rounded panel inside the card.
class Section final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Spotlight::Color::StrokeSubtle(), 1));
        painter.setBrush(Spotlight::Color::FillFaint());
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                Spotlight::Radius::Card, Spotlight::Radius::Card);
    }
};

// Shows a picture with a square selection: click or drag to move it.
class CropArea final : public QWidget {
public:
    CropArea(const QImage& image_, QWidget* parent) : QWidget(parent), image{image_} {
        const QSize fit = image.size().scaled(360, 360, Qt::KeepAspectRatio);
        setFixedSize(fit);
        scale = static_cast<double>(fit.width()) / image.width();
        side = std::min(image.width(), image.height());
        center = QPointF(image.width() / 2.0, image.height() / 2.0);
        setCursor(Qt::SizeAllCursor);
    }

    // 100: the whole short side; 400: a quarter of it.
    void SetZoom(int percent) {
        side = std::min(image.width(), image.height()) * 100.0 / percent;
        Clamp();
    }

    QRect Selection() const {
        const int s = static_cast<int>(side);
        const int x = std::clamp(static_cast<int>(center.x() - side / 2.0), 0, image.width() - s);
        const int y = std::clamp(static_cast<int>(center.y() - side / 2.0), 0, image.height() - s);
        return QRect(x, y, s, s);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath frame;
        frame.addRoundedRect(QRectF(rect()), Spotlight::Radius::Tile, Spotlight::Radius::Tile);
        painter.setClipPath(frame);
        painter.drawImage(rect(), image);
        const QRect s = Selection();
        const QRectF shown(s.x() * scale, s.y() * scale, s.width() * scale, s.height() * scale);
        QPainterPath outside;
        outside.addRect(QRectF(rect()));
        outside.addRoundedRect(shown, Spotlight::Radius::Tile, Spotlight::Radius::Tile);
        painter.fillPath(outside, QColor(0, 0, 0, 150));
        painter.setPen(QPen(Spotlight::Color::Accent(), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(shown.adjusted(1, 1, -1, -1), Spotlight::Radius::Tile,
                                Spotlight::Radius::Tile);
    }
    void mousePressEvent(QMouseEvent* event) override {
        last = event->position();
        pressed = event->position();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if ((event->buttons() & Qt::LeftButton) == 0) {
            return;
        }
        center += (event->position() - last) / scale;
        last = event->position();
        Clamp();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        // A simple click centers the square on the point.
        if (event->position() == pressed) {
            center = event->position() / scale;
            Clamp();
        }
    }

private:
    void Clamp() {
        center.setX(std::clamp(center.x(), side / 2.0, image.width() - side / 2.0));
        center.setY(std::clamp(center.y(), side / 2.0, image.height() - side / 2.0));
        update();
    }

    QImage image;
    double scale = 1.0;
    double side = 0.0;
    QPointF center;
    QPointF last;
    QPointF pressed;
};

class CropDialog final : public ArchiveCardDialog {
public:
    CropDialog(const QImage& image, QWidget* parent)
        : ArchiveCardDialog(tr("Crop the icon"), 440, parent) {
        SetHeader(nullptr, tr("Drag or click to move the square. Zoom in to keep a smaller part."));
        auto* section = AddSection({});
        area = new CropArea(image, card);
        section->addWidget(area, 0, Qt::AlignCenter);
        auto* zoom = new QSlider(Qt::Horizontal, card);
        zoom->setRange(100, 400);
        connect(zoom, &QSlider::valueChanged, area, [this](int value) { area->SetZoom(value); });
        AddRow(section, tr("Zoom"), zoom);
        AddButtons(tr("Use this square"), QStringLiteral("check"));
    }

    QRect Selection() const {
        return area->Selection();
    }

private:
    CropArea* area;
};

} // namespace

std::optional<QRect> ChooseIconCrop(const QImage& image, QWidget* parent) {
    if (image.width() == image.height()) {
        return image.rect();
    }
    CropDialog dialog(image, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.Selection();
}

QByteArray EncodeArchiveIcon(const QImage& source, const QRect& crop, int size) {
    const QImage scaled =
        source.copy(crop).scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage flat(size, size, QImage::Format_RGB32);
    flat.fill(Qt::black);
    QPainter painter(&flat);
    painter.drawImage(0, 0, scaled);
    painter.end();
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!flat.save(&buffer, "JPEG", 92)) {
        return {};
    }
    return bytes;
}

IconDropZone::IconDropZone(int size, QWidget* parent) : QWidget(parent) {
    setFixedSize(size, size);
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Drop an image here or click to choose one"));
}

void IconDropZone::SetIcon(const QPixmap& new_icon) {
    icon = new_icon;
    update();
}

void IconDropZone::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF area = QRectF(rect()).adjusted(1, 1, -1, -1);
    const qreal radius = Spotlight::Radius::Tile;
    const bool active = dragging || hovered;
    if (!icon.isNull()) {
        painter.drawPixmap(0, 0, RoundedPreview(icon, width()));
        painter.setPen(QPen(Spotlight::Color::StrokeSubtle(), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        if (active) {
            painter.setBrush(QColor(0, 0, 0, 130));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(area, radius, radius);
            painter.setPen(Spotlight::Color::Text());
            painter.setFont(Spotlight::Font(12, 600));
            painter.drawText(area, Qt::AlignCenter | Qt::TextWordWrap,
                             dragging ? tr("Drop to use") : tr("Change"));
        }
        return;
    }
    const auto accent = Spotlight::Color::Accent();
    QPen pen(active ? accent : Spotlight::Color::Stroke(), 1.5, Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(active ? Spotlight::Color::AccentSoft() : Spotlight::Color::FillFaint());
    painter.drawRoundedRect(area, radius, radius);
    const QPixmap glyph = Spotlight::IconPixmap(u"image", 24, Spotlight::Color::TextMuted(),
                                                devicePixelRatioF());
    painter.drawPixmap(QPointF((width() - 24) / 2.0, height() / 2.0 - 26), glyph);
    painter.setPen(Spotlight::Color::TextMuted());
    painter.setFont(Spotlight::Font(11, 500));
    painter.drawText(area.adjusted(6, height() / 2.0, -6, -6), Qt::AlignHCenter | Qt::AlignTop |
                                                                   Qt::TextWordWrap,
                     dragging ? tr("Release to use") : tr("Add an icon"));
}

void IconDropZone::dragEnterEvent(QDragEnterEvent* event) {
    if (!DroppedImage(event->mimeData()).isEmpty()) {
        dragging = true;
        event->acceptProposedAction();
        update();
    }
}

void IconDropZone::dragLeaveEvent(QDragLeaveEvent*) {
    dragging = false;
    update();
}

void IconDropZone::dropEvent(QDropEvent* event) {
    dragging = false;
    update();
    if (const auto path = DroppedImage(event->mimeData()); !path.isEmpty()) {
        event->acceptProposedAction();
        emit FileDropped(path);
    }
}

void IconDropZone::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        emit Clicked();
    }
}

void IconDropZone::enterEvent(QEnterEvent*) {
    hovered = true;
    update();
}

void IconDropZone::leaveEvent(QEvent*) {
    hovered = false;
    update();
}

ArchiveCardDialog::ArchiveCardDialog(const QString& title, int width, QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint) {
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(QString::fromLatin1(CardStyle));
    setWindowTitle(title);

    card = new QWidget(this);
    card->setObjectName(QStringLiteral("archiveCard"));
    card->setFixedWidth(width);
    body = new QVBoxLayout(card);
    body->setContentsMargins(24, 22, 24, 20);
    body->setSpacing(16);
    title_label = new QLabel(title, card);
    title_label->setFont(Spotlight::Font(20, 700));
    title_label->setWordWrap(true);
    Spotlight::SetTextColor(title_label, Spotlight::Color::Text());

    auto* center = new QVBoxLayout(this);
    center->addWidget(card, 0, Qt::AlignCenter);
}

QLabel* ArchiveCardDialog::SetHeader(QWidget* icon, const QString& subtitle,
                                     const std::vector<QAbstractButton*>& pills) {
    auto* header = new QHBoxLayout;
    header->setSpacing(16);
    if (icon != nullptr) {
        header->addWidget(icon, 0, Qt::AlignTop);
    }
    auto* titles = new QVBoxLayout;
    titles->setSpacing(4);
    titles->addWidget(title_label);
    auto* sub = TextLabel(subtitle, 13, 400, Spotlight::Color::TextSecondary());
    sub->setWordWrap(true);
    sub->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(sub);
    if (!pills.empty()) {
        titles->addSpacing(6);
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        for (auto* pill : pills) {
            row->addWidget(pill);
        }
        row->addStretch();
        titles->addLayout(row);
    }
    titles->addStretch();
    header->addLayout(titles, 1);
    body->insertLayout(0, header);
    return sub;
}

QVBoxLayout* ArchiveCardDialog::AddSection(const QString& title) {
    auto* group = new QVBoxLayout;
    group->setSpacing(8);
    if (!title.isEmpty()) {
        auto* label = TextLabel(title.toUpper(), 11, 600, Spotlight::Color::TextMuted());
        label->setContentsMargins(4, 0, 0, 0);
        group->addWidget(label);
    }
    auto* section = new Section(card);
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    group->addWidget(section);
    body->addLayout(group);
    return layout;
}

void ArchiveCardDialog::AddRow(QVBoxLayout* section, const QString& label, QWidget* field) {
    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    auto* text = TextLabel(label, 13, 500, Spotlight::Color::TextSecondary());
    text->setFixedWidth(LabelWidth);
    row->addWidget(text);
    row->addWidget(field, 1);
    section->addLayout(row);
}

QLabel* ArchiveCardDialog::Hint(const QString& text) {
    auto* label = TextLabel(text, 12, 400, Spotlight::Color::TextMuted());
    label->setWordWrap(true);
    return label;
}

Spotlight::PillButton* ArchiveCardDialog::AddButtons(const QString& action,
                                                     const QString& action_icon) {
    auto* row = new QHBoxLayout;
    row->setSpacing(10);
    row->addStretch();
    auto* cancel_button = new Spotlight::PillButton(tr("Cancel"), {},
                                                    Spotlight::PillButton::Kind::Control, 38, card);
    action_button = new Spotlight::PillButton(action, action_icon,
                                              Spotlight::PillButton::Kind::Accent, 38, card);
    row->addWidget(cancel_button);
    row->addWidget(action_button);
    body->addSpacing(4);
    body->addLayout(row);
    connect(cancel_button, &QAbstractButton::clicked, this, &QDialog::reject);
    connect(action_button, &QAbstractButton::clicked, this, &QDialog::accept);
    return action_button;
}

Spotlight::PillButton* ArchiveCardDialog::SmallPill(const QString& text, const QString& icon) {
    return new Spotlight::PillButton(text, icon, Spotlight::PillButton::Kind::Control, 30);
}

void ArchiveCardDialog::showEvent(QShowEvent* event) {
    if (auto* window = parentWidget() ? parentWidget()->window() : nullptr) {
        setGeometry(window->geometry());
    } else {
        adjustSize();
    }
    QDialog::showEvent(event);
    Spotlight::FadeIn(card);
}

void ArchiveCardDialog::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), QColor(0x06, 0x07, 0x0a, 0x8c));
    painter.setPen(QPen(Spotlight::Color::StrokeSubtle(), 1));
    painter.setBrush(Spotlight::Color::Popover());
    painter.drawRoundedRect(QRectF(card->geometry()).adjusted(0.5, 0.5, -0.5, -0.5),
                            Spotlight::Radius::InGamePanel, Spotlight::Radius::InGamePanel);
}

void ArchiveCardDialog::keyPressEvent(QKeyEvent* event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        action_button != nullptr) {
        if (action_button->isEnabled()) {
            action_button->click();
        }
        return;
    }
    QDialog::keyPressEvent(event);
}

ArchiveMessageDialog::ArchiveMessageDialog(const QString& title, const QString& text,
                                           QWidget* parent)
    : ArchiveCardDialog(title, 480, parent) {
    body->addWidget(title_label);
    auto* message = TextLabel(text, 13, 400, Spotlight::Color::TextSecondary());
    message->setWordWrap(true);
    message->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(message);
}

QCheckBox* ArchiveMessageDialog::AddCheckBox(const QString& text, bool checked) {
    if (checks == nullptr) {
        checks = AddSection({});
    }
    auto* box = new QCheckBox(text, card);
    box->setChecked(checked);
    checks->addWidget(box);
    return box;
}

QAbstractButton* ArchiveMessageDialog::AddButton(const QString& text, const QString& icon,
                                                 bool accent) {
    if (buttons == nullptr) {
        buttons = new QHBoxLayout;
        buttons->setSpacing(10);
        buttons->addStretch();
        body->addSpacing(4);
        body->addLayout(buttons);
    }
    auto* button = new Spotlight::PillButton(text, icon,
                                             accent ? Spotlight::PillButton::Kind::Accent
                                                    : Spotlight::PillButton::Kind::Control,
                                             38, card);
    buttons->addWidget(button);
    if (accent) {
        action_button = button;
    }
    connect(button, &QAbstractButton::clicked, this, [this, button, accent] {
        clicked = button;
        accent ? accept() : reject();
    });
    return button;
}

void ArchiveNotice(QWidget* parent, const QString& title, const QString& text) {
    ArchiveMessageDialog dialog(title, text, parent);
    dialog.AddButton(QObject::tr("OK"), {}, true);
    dialog.exec();
}

NupConvertDialog::NupConvertDialog(NupConvertSummary summary_, QWidget* parent)
    : ArchiveCardDialog(tr("Convert to NUP"), 580, parent), summary{std::move(summary_)},
      archive{summary.archive} {
    const auto& detected = summary.detected;
    icon_preview = new IconDropZone(HeaderIconSize, card);
    connect(icon_preview, &IconDropZone::Clicked, this, &NupConvertDialog::ChooseIcon);
    connect(icon_preview, &IconDropZone::FileDropped, this, &NupConvertDialog::SetIconFromFile);
    auto* choose = SmallPill(tr("Choose image"), QStringLiteral("image"));
    reset_icon = SmallPill(tr("Use the game's icon"), QStringLiteral("arrow-counter-clockwise"));
    connect(choose, &QAbstractButton::clicked, this, &NupConvertDialog::ChooseIcon);
    connect(reset_icon, &QAbstractButton::clicked, this, &NupConvertDialog::ResetIcon);
    SetHeader(icon_preview, summary.source_name, {choose, reset_icon});

    auto* details = AddSection(tr("Shown in the library"));
    name_edit = new QLineEdit(QString::fromStdString(detected.title_name), card);
    name_edit->setPlaceholderText(tr("Game name"));
    name_edit->setClearButtonEnabled(true);
    publisher_edit = new QLineEdit(QString::fromStdString(detected.publisher), card);
    publisher_edit->setPlaceholderText(tr("Publisher"));
    publisher_edit->setClearButtonEnabled(true);
    version_edit = new QLineEdit(QString::fromStdString(detected.display_version), card);
    version_edit->setPlaceholderText(tr("e.g. 1.2.0"));
    AddRow(details, tr("Name"), name_edit);
    AddRow(details, tr("Publisher"), publisher_edit);
    AddRow(details, tr("Version"), version_edit);
    if (!detected.version_source.empty()) {
        details->addWidget(Hint(tr("The version was read from the %1.")
                                    .arg(QString::fromStdString(detected.version_source))));
    }

    auto* contents = AddSection(tr("Packed into the NUP"));
    for (const auto& package : summary.packages) {
        auto* row = new QHBoxLayout;
        row->setSpacing(12);
        auto* texts = new QVBoxLayout;
        texts->setSpacing(1);
        texts->addWidget(TextLabel(package.label, 13, 600, Spotlight::Color::Text()));
        if (!package.detail.isEmpty()) {
            texts->addWidget(TextLabel(package.detail, 12, 400, Spotlight::Color::TextMuted()));
        }
        row->addLayout(texts, 1);
        row->addWidget(TextLabel(HumanSize(package.bytes), 12, 500,
                                 Spotlight::Color::TextSecondary()));
        if (package.optional) {
            auto* toggle = new Spotlight::ToggleSwitch(card);
            toggle->setChecked(true);
            toggle->setAccessibleName(package.label);
            connect(toggle, &QAbstractButton::toggled, this, &NupConvertDialog::UpdateSpace);
            row->addWidget(toggle);
            package_toggles.push_back(toggle);
        } else {
            package_toggles.push_back(nullptr);
        }
        contents->addLayout(row);
    }

    auto* target = AddSection(tr("Save as"));
    auto* target_row = new QHBoxLayout;
    target_row->setSpacing(12);
    target_label = TextLabel({}, 13, 500, Spotlight::Color::Text());
    target_label->setWordWrap(true);
    target_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* change = SmallPill(tr("Change"), QStringLiteral("folder-open"));
    connect(change, &QAbstractButton::clicked, this, &NupConvertDialog::ChooseTarget);
    target_row->addWidget(target_label, 1);
    target_row->addWidget(change, 0, Qt::AlignTop);
    target->addLayout(target_row);
    space_label = Hint({});
    target->addWidget(space_label);
    if (summary.offer_low_storage) {
        auto* row = new QHBoxLayout;
        auto* texts = new QVBoxLayout;
        texts->setSpacing(1);
        texts->addWidget(TextLabel(tr("Low storage"), 13, 600, Spotlight::Color::Text()));
        texts->addWidget(
            Hint(tr("Deletes each loose file once it is in the NUP. This cannot be undone.")));
        row->addLayout(texts, 1);
        low_storage = new Spotlight::ToggleSwitch(card);
        connect(low_storage, &QAbstractButton::toggled, this, &NupConvertDialog::UpdateSpace);
        row->addWidget(low_storage);
        target->addLayout(row);
    }

    AddButtons(tr("Convert"), QStringLiteral("archive"));
    UpdateIconPreview();
    UpdateSpace();
}

NupConvertDialog::~NupConvertDialog() = default;

FileSys::Nup::Metadata NupConvertDialog::Metadata() const {
    const auto& detected = summary.detected;
    FileSys::Nup::Metadata result;
    if (const auto name = name_edit->text().trimmed().toStdString(); name != detected.title_name) {
        result.title_name = name;
    }
    if (const auto publisher = publisher_edit->text().trimmed().toStdString();
        publisher != detected.publisher) {
        result.publisher = publisher;
    }
    if (const auto version = version_edit->text().trimmed().toStdString();
        version != detected.display_version) {
        result.display_version = version;
    }
    if (!custom_icon.isEmpty()) {
        result.icon.assign(custom_icon.begin(), custom_icon.end());
    }
    return result;
}

bool NupConvertDialog::DeleteSources() const {
    return low_storage != nullptr && low_storage->isChecked();
}

std::vector<bool> NupConvertDialog::IncludedPackages() const {
    std::vector<bool> included;
    for (const auto* toggle : package_toggles) {
        included.push_back(toggle == nullptr || toggle->isChecked());
    }
    return included;
}

std::filesystem::path NupConvertDialog::Archive() const {
    return archive;
}

QString NupConvertDialog::DisplayName() const {
    return name_edit->text().trimmed();
}

QPixmap NupConvertDialog::CurrentIcon() const {
    return current_icon;
}

void NupConvertDialog::ChooseIcon() {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Choose Icon"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
    if (!path.isEmpty()) {
        SetIconFromFile(path);
    }
}

void NupConvertDialog::SetIconFromFile(const QString& path) {
    const QImage image = ReadImage(path);
    if (image.isNull()) {
        return;
    }
    const auto crop = ChooseIconCrop(image, this);
    if (!crop) {
        return;
    }
    custom_icon = EncodeArchiveIcon(image, *crop);
    UpdateIconPreview();
}

void NupConvertDialog::ResetIcon() {
    custom_icon.clear();
    UpdateIconPreview();
}

void NupConvertDialog::UpdateIconPreview() {
    QPixmap pixmap;
    if (!custom_icon.isEmpty()) {
        pixmap.loadFromData(custom_icon);
    } else {
        pixmap = PixmapFromBytes(summary.detected.icon);
    }
    current_icon = pixmap;
    reset_icon->setVisible(!custom_icon.isEmpty() && !summary.detected.icon.empty());
    icon_preview->SetIcon(pixmap);
}

void NupConvertDialog::ChooseTarget() {
    QString chosen = QFileDialog::getSaveFileName(this, tr("Save the NUP as"), ToQString(archive),
                                                  tr("NUP archive (*.nup)"));
    if (chosen.isEmpty()) {
        return;
    }
    if (!chosen.endsWith(QStringLiteral(".nup"), Qt::CaseInsensitive)) {
        chosen += QStringLiteral(".nup");
    }
    archive = FromQString(chosen);
    UpdateSpace();
}

void NupConvertDialog::UpdateSpace() {
    target_label->setText(ToQString(archive));
    u64 needed = summary.extra_bytes;
    if (!DeleteSources()) {
        const auto included = IncludedPackages();
        for (size_t i = 0; i < summary.packages.size(); ++i) {
            if (included[i]) {
                needed += summary.packages[i].bytes;
            }
        }
    }
    const u64 available = Common::FS::GetFreeSpaceSize(archive.parent_path());
    std::error_code ec;
    QString problem;
    if (std::filesystem::exists(archive, ec)) {
        problem = tr("A file with this name already exists.");
    } else if (available < needed) {
        problem = tr("Not enough free space on this drive.");
    }
    QString text = tr("Needs about %1 · %2 free on this drive.")
                       .arg(HumanSize(needed), HumanSize(available));
    if (!problem.isEmpty()) {
        text = problem + QLatin1Char(' ') + text;
    }
    space_label->setText(text);
    Spotlight::SetTextColor(space_label, problem.isEmpty() ? Spotlight::Color::TextMuted()
                                                           : Spotlight::Color::Warning());
    if (action_button != nullptr) {
        action_button->setEnabled(problem.isEmpty());
    }
}

ArchiveEditDialog::ArchiveEditDialog(Format format_, const std::filesystem::path& archive,
                                     FileSys::Nup::Metadata current_,
                                     std::vector<u8> original_icon_, QWidget* parent)
    : ArchiveCardDialog(tr("Edit title and icon"), 580, parent), format{format_},
      current{std::move(current_)}, original_icon{std::move(original_icon_)} {
    const bool nup = format == Format::Nup;
    icon_preview = new IconDropZone(HeaderIconSize, card);
    connect(icon_preview, &IconDropZone::Clicked, this, &ArchiveEditDialog::ChooseIcon);
    connect(icon_preview, &IconDropZone::FileDropped, this, &ArchiveEditDialog::SetIconFromFile);
    auto* choose = SmallPill(tr("Choose image"), QStringLiteral("image"));
    find_icon = SmallPill(tr("Find online"), QStringLiteral("magnifying-glass"));
    auto* clear = SmallPill(tr("Remove"), QStringLiteral("trash"));
    connect(choose, &QAbstractButton::clicked, this, &ArchiveEditDialog::ChooseIcon);
    connect(find_icon, &QAbstractButton::clicked, this, &ArchiveEditDialog::FindIcon);
    connect(clear, &QAbstractButton::clicked, this, &ArchiveEditDialog::ClearIcon);
    std::vector<QAbstractButton*> pills{choose, find_icon};
    if (nup) {
        pills.push_back(clear);
    } else {
        clear->hide();
    }
    if (!original_icon.empty() && original_icon != current.icon) {
        auto* restore = SmallPill(tr("Original"), QStringLiteral("arrow-counter-clockwise"));
        restore->setToolTip(tr("Use the icon the archive was made with"));
        connect(restore, &QAbstractButton::clicked, this, &ArchiveEditDialog::RestoreIcon);
        pills.push_back(restore);
    }
    SetHeader(icon_preview, ToQString(archive.filename()), pills);
    icon_hint = Hint({});
    body->addWidget(icon_hint);

    icon_search = new IconSearch(this);
    connect(icon_search, &IconSearch::RunningChanged, this, [this](bool running) {
        find_icon->setEnabled(!running);
        find_icon->setText(running ? tr("Searching…") : tr("Find online"));
    });
    connect(icon_search, &IconSearch::IconSelected, this, &ArchiveEditDialog::OnIconFound);

    auto* details = AddSection(tr("Shown in the library"));
    name_edit = new QLineEdit(QString::fromStdString(current.title_name), card);
    name_edit->setPlaceholderText(tr("Game name"));
    name_edit->setClearButtonEnabled(true);
    if (nup) {
        auto* name_row = new QWidget(card);
        auto* name_layout = new QHBoxLayout(name_row);
        name_layout->setContentsMargins(0, 0, 0, 0);
        name_layout->setSpacing(8);
        name_layout->addWidget(name_edit, 1);
        auto* languages = new Spotlight::CircleButton(QStringLiteral("translate"), 34, card);
        languages->setCheckable(true);
        name_layout->addWidget(languages);
        AddRow(details, tr("Name"), name_row);

        localized_panel = new QScrollArea(card);
        auto* scroll = static_cast<QScrollArea*>(localized_panel);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setMaximumHeight(230);
        auto* inner = new QWidget;
        auto* inner_layout = new QVBoxLayout(inner);
        inner_layout->setContentsMargins(0, 0, 8, 0);
        inner_layout->setSpacing(8);
        int localized_set = 0;
        for (const auto& language : LocalizedLanguages) {
            auto* field = new QLineEdit(inner);
            field->setClearButtonEnabled(true);
            for (const auto& [tag, name] : current.localized_titles) {
                if (tag == language.tag) {
                    field->setText(QString::fromStdString(name));
                    ++localized_set;
                }
            }
            auto* row = new QHBoxLayout;
            auto* label = TextLabel(tr(language.name), 12, 500, Spotlight::Color::TextMuted());
            label->setFixedWidth(150);
            row->addWidget(label);
            row->addWidget(field, 1);
            inner_layout->addLayout(row);
            localized_edits.emplace_back(language.tag, field);
        }
        scroll->setWidget(inner);
        details->addWidget(localized_panel);
        localized_panel->setVisible(false);
        languages->setToolTip(localized_set > 0 ? tr("Names per language (%1 set)").arg(localized_set)
                                                : tr("Names per language"));
        connect(languages, &QAbstractButton::toggled, this, [this](bool shown) {
            localized_panel->setVisible(shown);
            card->adjustSize();
            update();
        });
        const auto update_placeholders = [this] {
            const QString fallback = name_edit->text().trimmed();
            for (const auto& [tag, field] : localized_edits) {
                field->setPlaceholderText(fallback.isEmpty() ? tr("Same as Name") : fallback);
            }
        };
        connect(name_edit, &QLineEdit::textChanged, this, update_placeholders);
        update_placeholders();
    } else {
        AddRow(details, tr("Name"), name_edit);
    }
    publisher_edit = new QLineEdit(QString::fromStdString(current.publisher), card);
    publisher_edit->setPlaceholderText(tr("Publisher"));
    publisher_edit->setClearButtonEnabled(true);
    AddRow(details, tr("Publisher"), publisher_edit);
    if (nup) {
        version_edit = new QLineEdit(QString::fromStdString(current.display_version), card);
        version_edit->setPlaceholderText(tr("e.g. 1.2.0"));
        version_edit->setClearButtonEnabled(true);
        AddRow(details, tr("Version"), version_edit);
    } else {
        icon_size = new Spotlight::SegmentedControl(
            {QStringLiteral("256"), QStringLiteral("512"), QStringLiteral("1024")},
            Spotlight::SegmentedControl::Kind::Accent, 34, card);
        icon_size->setToolTip(tr("Pixels per side of the icon stored in the archive"));
        connect(icon_size, &Spotlight::SegmentedControl::CurrentChanged, this,
                &ArchiveEditDialog::EncodeIcon);
        auto* size_row = new QWidget(card);
        auto* size_layout = new QHBoxLayout(size_row);
        size_layout->setContentsMargins(0, 0, 0, 0);
        size_layout->addWidget(icon_size);
        size_layout->addStretch();
        AddRow(details, tr("Icon size"), size_row);
    }

    auto* saved = AddSection(tr("Saved in the file"));
    std::error_code ec;
    const auto size = std::filesystem::file_size(archive, ec);
    saved->addWidget(
        Hint(nup ? tr("Written into the NUP itself, so it shows on every PC. The file (%1) is "
                      "copied once with the new details; the original stays untouched until the "
                      "copy is complete, so Cancel or an error never damages it.")
                       .arg(HumanSize(ec ? 0 : size))
                 : tr("Written into the NX archive itself, so it shows on every PC. Game data is "
                      "not touched.")));

    AddButtons(tr("Save to file"), QStringLiteral("check"));
    UpdateIconPreview();
}

ArchiveEditDialog::~ArchiveEditDialog() = default;

FileSys::Nup::MetadataEdit ArchiveEditDialog::Edit() const {
    FileSys::Nup::MetadataEdit edit;
    if (remove_icon) {
        edit.icon = std::vector<u8>{};
    } else if (!custom_icon.isEmpty()) {
        edit.icon = std::vector<u8>(custom_icon.begin(), custom_icon.end());
    }
    if (const auto name = name_edit->text().trimmed().toStdString(); name != current.title_name) {
        edit.title_name = name;
    }
    if (const auto publisher = publisher_edit->text().trimmed().toStdString();
        publisher != current.publisher) {
        edit.publisher = publisher;
    }
    if (format == Format::Nx) {
        return edit;
    }
    // Tags the console has no field for stay as stored.
    std::vector<std::pair<std::string, std::string>> localized;
    for (const auto& entry : current.localized_titles) {
        if (!IsEditableLanguage(entry.first)) {
            localized.push_back(entry);
        }
    }
    for (const auto& [tag, field] : localized_edits) {
        if (const auto name = field->text().trimmed().toStdString(); !name.empty()) {
            localized.emplace_back(tag, name);
        }
    }
    auto stored = current.localized_titles;
    auto wanted = localized;
    std::ranges::sort(stored);
    std::ranges::sort(wanted);
    if (stored != wanted) {
        edit.localized_titles = std::move(localized);
    }
    if (const auto version = version_edit->text().trimmed().toStdString();
        version != current.display_version) {
        edit.display_version = version;
    }
    return edit;
}

QString ArchiveEditDialog::DisplayName() const {
    return name_edit->text().trimmed();
}

QPixmap ArchiveEditDialog::CurrentIcon() const {
    return current_icon;
}

void ArchiveEditDialog::ChooseIcon() {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Choose Icon"), {}, tr("Images (*.png *.jpg *.jpeg *.bmp *.webp);;All Files (*)"));
    if (!path.isEmpty()) {
        SetIconFromFile(path);
    }
}

void ArchiveEditDialog::UseImage(const QImage& image, bool crop) {
    if (image.isNull()) {
        icon_hint->setText(tr("That image could not be read."));
        return;
    }
    QRect square = CenterSquare(image);
    if (crop) {
        const auto chosen = ChooseIconCrop(image, this);
        if (!chosen) {
            return;
        }
        square = *chosen;
    }
    icon_source = image;
    icon_crop = square;
    remove_icon = false;
    EncodeIcon();
}

void ArchiveEditDialog::SetIconFromFile(const QString& path) {
    UseImage(ReadImage(path), true);
}

void ArchiveEditDialog::FindIcon() {
    icon_search->Start(name_edit->text());
}

void ArchiveEditDialog::OnIconFound(const QByteArray& png, const QString& resolved_title) {
    QImage image;
    image.loadFromData(png);
    UseImage(image, false);
    if (!resolved_title.isEmpty() && !image.isNull()) {
        name_edit->setText(resolved_title);
    }
}

void ArchiveEditDialog::ClearIcon() {
    icon_source = {};
    custom_icon.clear();
    remove_icon = true;
    UpdateIconPreview();
}

void ArchiveEditDialog::RestoreIcon() {
    icon_source = {};
    remove_icon = false;
    custom_icon = QByteArray(reinterpret_cast<const char*>(original_icon.data()),
                             static_cast<qsizetype>(original_icon.size()));
    UpdateIconPreview();
}

void ArchiveEditDialog::EncodeIcon() {
    if (icon_source.isNull()) {
        return;
    }
    const int size = icon_size != nullptr ? 256 << std::max(0, icon_size->CurrentIndex()) : IconSize;
    custom_icon = EncodeArchiveIcon(icon_source, icon_crop, size);
    if (custom_icon.isEmpty()) {
        icon_hint->setText(tr("That image could not be converted."));
        return;
    }
    UpdateIconPreview();
}

void ArchiveEditDialog::UpdateIconPreview() {
    QPixmap pixmap;
    const QString kind = format == Format::Nup ? tr("NUP") : tr("NX archive");
    if (remove_icon) {
        icon_hint->setText(tr("The icon will be removed from the %1.").arg(kind));
    } else if (!custom_icon.isEmpty()) {
        pixmap.loadFromData(custom_icon);
        icon_hint->setText(tr("New icon: written into the %1 as a %2 × %2 JPEG when you save.")
                               .arg(kind)
                               .arg(pixmap.width()));
    } else if (!current.icon.empty()) {
        pixmap = PixmapFromBytes(current.icon);
        icon_hint->setText(tr("The icon stored in the %1. Drop an image on it to replace it.")
                               .arg(kind));
    } else {
        icon_hint->setText(tr("The %1 has no icon. Drop an image on the square to add one.")
                               .arg(kind));
    }
    current_icon = pixmap;
    icon_preview->SetIcon(pixmap);
}

NupProgressDialog::NupProgressDialog(const QString& operation, const QString& title,
                                     const QPixmap& icon, QWidget* parent)
    : ArchiveCardDialog(title.isEmpty() ? operation : title, 500, parent) {
    setWindowTitle(operation);
    setModal(true);
    QLabel* icon_label = nullptr;
    if (!icon.isNull()) {
        icon_label = new QLabel(card);
        icon_label->setFixedSize(64, 64);
        icon_label->setPixmap(RoundedPreview(icon, 64));
    }
    step = operation;
    status = SetHeader(icon_label, operation);

    bar = new QProgressBar(card);
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    body->addWidget(bar);
    details = Hint(tr("Preparing…"));
    details->setMinimumHeight(QFontMetrics(details->font()).height() * 2 + 4);
    body->addWidget(details);

    auto* row = new QHBoxLayout;
    row->addStretch();
    cancel = new Spotlight::PillButton(tr("Cancel"), QStringLiteral("x"),
                                       Spotlight::PillButton::Kind::Control, 38, card);
    connect(cancel, &QAbstractButton::clicked, this, &NupProgressDialog::CancelRequested);
    row->addWidget(cancel);
    body->addLayout(row);
    timer.start();
}

void NupProgressDialog::SetStep(const QString& new_step) {
    step = new_step;
    status->setText(step);
    bar->setValue(0);
    timer.restart();
}

void NupProgressDialog::SetProgress(const FileSys::Nup::PackProgress& progress) {
    if (cancelling) {
        return;
    }
    const double fraction =
        progress.bytes_total == 0 ? 0.0 : double(progress.bytes_done) / progress.bytes_total;
    bar->setValue(static_cast<int>(fraction * 1000));
    status->setText(tr("%1 · %2%").arg(step).arg(fraction * 100, 0, 'f', 0));
    QString text = tr("%1 of %2").arg(HumanSize(progress.bytes_done),
                                      HumanSize(progress.bytes_total));
    if (progress.files_total != 0) {
        text += tr(" · %1 of %2 files").arg(progress.files_done).arg(progress.files_total);
    }
    const double seconds = timer.elapsed() / 1000.0;
    if (seconds >= 3.0 && progress.bytes_done > 0 && fraction < 1.0) {
        const double rate = progress.bytes_done / seconds;
        const int remaining = static_cast<int>((progress.bytes_total - progress.bytes_done) / rate);
        text += tr("\n%1 MB/s · about %2:%3 left")
                    .arg(rate / (1024 * 1024), 0, 'f', 0)
                    .arg(remaining / 60)
                    .arg(remaining % 60, 2, 10, QLatin1Char('0'));
    }
    details->setText(text);
}

void NupProgressDialog::SetCancelling() {
    cancelling = true;
    cancel->setEnabled(false);
    status->setText(tr("Cancelling…"));
    bar->setRange(0, 0);
}

void NupProgressDialog::Finish() {
    finished = true;
    close();
}

void NupProgressDialog::closeEvent(QCloseEvent* event) {
    // Only the window code closes this dialog; a close request cancels the job.
    if (!finished) {
        emit CancelRequested();
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void NupProgressDialog::reject() {
    if (!finished) {
        emit CancelRequested();
        return;
    }
    QDialog::reject();
}
