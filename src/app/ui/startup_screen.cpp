#include "ui/startup_screen.h"

#include "ui/theme.h"

#include <QGuiApplication>
#include <QLabel>
#include <QPixmap>
#include <QProgressBar>
#include <QScreen>
#include <QVBoxLayout>

namespace denso::ui {

StartupScreen::StartupScreen(bool dark, QWidget* parent) : QWidget(parent) {
    setWindowFlag(Qt::FramelessWindowHint);
    setWindowTitle(QStringLiteral("Denso DigitalReader"));

    const Palette p = denso::ui::palette(dark);
    setStyleSheet(
        QStringLiteral(
            "QWidget { background: %1; color: %2; }"
            "QLabel#title { font-size: 20px; font-weight: 600; }"
            "QLabel#status { color: %3; }"
            "QProgressBar { border: none; background: %4;"
            " border-radius: 4px; height: 6px; }"
            "QProgressBar::chunk { background: %5; border-radius: 4px; }")
            .arg(p.panel_2.name(), p.txt.name(), p.txt_faint.name(),
                 p.panel_3.name(), p.gold.name()));

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(48, 40, 48, 40);
    col->setSpacing(16);
    col->setAlignment(Qt::AlignCenter);

    auto* logo = new QLabel;
    const QPixmap pix(QStringLiteral(":/icon.png"));
    if (!pix.isNull()) {
        logo->setPixmap(pix.scaled(96, 96, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation));
    }
    logo->setAlignment(Qt::AlignCenter);

    auto* title = new QLabel(QStringLiteral("Denso DigitalReader"));
    title->setObjectName(QStringLiteral("title"));
    title->setAlignment(Qt::AlignCenter);

    status_ = new QLabel(QStringLiteral("Starting…"));
    status_->setObjectName(QStringLiteral("status"));
    status_->setAlignment(Qt::AlignCenter);

    auto* bar = new QProgressBar;
    bar->setRange(0, 0);  // indeterminate — animates on the event loop
    bar->setTextVisible(false);
    bar->setFixedWidth(240);

    col->addWidget(logo);
    col->addWidget(title);
    col->addWidget(status_);
    col->addWidget(bar, 0, Qt::AlignHCenter);

    setFixedSize(360, 300);
    if (QScreen* s = QGuiApplication::primaryScreen()) {
        const QRect g = s->geometry();
        move(g.center() - rect().center());
    }
}

void StartupScreen::set_status(const QString& msg) { status_->setText(msg); }

} // namespace denso::ui
