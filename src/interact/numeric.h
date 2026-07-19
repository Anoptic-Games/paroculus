// Typed values during a gesture.
//
// Every gesture has a numeric twin: start drawing, type 45, Enter, and the
// length under adjustment becomes exactly 45. Approximate gesture and exact
// entry are two entrances to the same edit, never two tools — which is why this
// drives the tool that is already running rather than opening a dialog that
// replaces it.
//
// Two hygiene rules from PRINCIPLES are load-bearing here:
//
//   full precision   an edit opens on the stored value, never on the string
//                    that was rendered from it. format() is lossy by design, so
//                    a round trip through the strip would quietly truncate the
//                    number the user is editing.
//   one boundary     text becomes millimetres here, through core/units, and
//                    nothing above this holds a value in display units.
#pragma once

#include <optional>
#include <string>

#include "core/units.h"

namespace paroculus {

// One entry session: which parameter is being typed into, and what has been
// typed so far. Deliberately holds text rather than a number — a half-typed
// "4." is a real state the user is in, and rounding it to 4 mid-keystroke
// would fight them.
class NumericEntry {
public:
    bool active() const { return active_; }
    size_t target() const { return target_; }
    const std::string &text() const { return text_; }

    // Opens on `target`, empty. Not prefilled with the current value: the strip
    // shows a rounded number, and prefilling would make the rendered string the
    // thing being edited rather than the value.
    void begin(size_t target);
    void retarget(size_t target);
    void type(char c);
    void backspace();
    void cancel();

    // The typed value in document units, or nullopt while the text is not yet a
    // complete number. Unit suffixes are honoured, so "2cm" is 20.
    std::optional<double> value() const;

private:
    bool active_ = false;
    size_t target_ = 0;
    std::string text_;
};

}  // namespace paroculus
