// The Camera dialog's first page: the list of configured cameras, each row with
// Configure / Areas / Delete. Owns its own DB reads (list + delete); the
// coordinator drives navigation by listening to the request signals.
#pragma once

#include "camera/camera.h"

#include <QSqlDatabase>
#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace denso::ui {

class CameraListPage : public QWidget {
    Q_OBJECT

public:
    explicit CameraListPage(QSqlDatabase db, QWidget* parent = nullptr);

    /// Re-read the cameras and rebuild the rows.
    void reload();

signals:
    void add_requested();
    void configure_requested(const camera::Camera& cam);
    void areas_requested(const camera::Camera& cam);
    void changed();  // a camera was deleted here

private:
    QSqlDatabase db_;
    QVBoxLayout* rows_box_ = nullptr;
    QLabel* empty_label_ = nullptr;
};

} // namespace denso::ui
