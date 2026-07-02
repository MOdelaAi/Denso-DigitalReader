// A frameless, centered startup splash shown while detection engines warm up
// (see ui/startup.{h,cpp}). Self-styled from the theme palette because the
// app-wide stylesheet (qApp->setStyleSheet) is only applied later, in
// MainWindow::apply_startup. set_status() updates the progress line; an
// indeterminate progress bar animates on the main event loop as a liveness cue.
#pragma once

#include <QWidget>

class QLabel;

namespace denso::ui {

class StartupScreen : public QWidget {
    Q_OBJECT

public:
    explicit StartupScreen(bool dark, QWidget* parent = nullptr);

public slots:
    void set_status(const QString& msg);

private:
    QLabel* status_ = nullptr;
};

} // namespace denso::ui
