// Direct numeric entry for a reporting-zone number, shared by the digit Areas
// step and the Ball Leveler calibration step.
//
// It replaced a combo box listing every legal number. That listing was workable
// at 12 numbers and is not at 100: a zone id is an IDENTIFIER the operator
// already knows from the machine, not an index to be hunted down a dropdown.
// So the operator types it, and the widget's job becomes telling them
// immediately whether what they typed is usable — and if not, why.
//
// The widget owns the parse rule for the WHOLE application, which is why both
// pages use it rather than each spelling out "digits only, 1..99". It is UX
// only: camera::replace_areas and level::save_level_configuration re-enforce
// the same range authoritatively, because this class is not on the only path
// into either.
#pragma once

#include <QString>
#include <QWidget>

#include <map>
#include <optional>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace denso::ui {

class ZoneNumberEdit : public QWidget {
    Q_OBJECT

public:
    /// `allow_unassigned` — a digit ROI may be detection-only (no zone), so it
    /// gets the "report this area" checkbox; a Ball zone must always carry a
    /// number, so it does not. Unassigned is expressed by that checkbox and
    /// nothing else — 0 is not a way to spell it, it is simply refused as out
    /// of range like 100 is.
    explicit ZoneNumberEdit(bool allow_unassigned, QWidget* parent = nullptr);

    /// zone -> who holds it, EXCLUDING the target being edited (so re-saving a
    /// zone's own number never reads as a duplicate). Drives both the "Used
    /// zones" line and the duplicate warning.
    void set_unavailable(std::map<int, QString> unavailable);

    /// Display `zone` without emitting zone_changed — for loading a selection.
    void set_zone(std::optional<int> zone);

    /// The last VALID value entered. nullopt means unassigned. While the typed
    /// text is invalid this holds the previous value; ask is_valid() before
    /// trusting it to describe what is on screen.
    std::optional<int> zone() const { return zone_; }

    /// False while the typed text is unusable — malformed, out of range,
    /// duplicated, or empty when a number is required. Save/Apply must be gated
    /// on this, not merely on the absence of a conflict.
    bool is_valid() const { return problem_.isEmpty(); }

    /// The operator-facing sentence for the current problem, or empty.
    QString problem() const { return problem_; }

signals:
    /// Emitted only when the typed text parses to a usable value (or the area
    /// is switched to detection-only). An invalid keystroke never writes a
    /// half-typed number into the model behind it.
    void zone_changed(std::optional<int> zone);

private:
    void revalidate();          // parse -> problem_/zone_, then repaint + emit
    void refresh_used_label();

    bool allow_unassigned_ = true;
    QCheckBox* report_check_ = nullptr;   // null when !allow_unassigned_
    QLineEdit* edit_ = nullptr;
    QLabel* used_ = nullptr;
    QLabel* warning_ = nullptr;

    std::map<int, QString> unavailable_;
    std::optional<int> zone_;
    QString problem_;
    bool syncing_ = false;   // suppresses zone_changed while set_zone() paints
};

} // namespace denso::ui
