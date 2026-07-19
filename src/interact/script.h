// Gesture scripts: a session as a value.
//
// A script is a starting document plus the exact sequence of abstract events
// that were fed to a Session. Because interact is toolkit-free, replaying one
// needs no window, no Skia and no Qt — which is what lets the same recording
// serve three jobs that would otherwise need three artefacts:
//
//   in CI          the corpus drives scripts headlessly and asserts invariants
//   by hand        `--script` plays one back in the shell so a person can watch
//   in review      a feel discovery session is a file, not a memory
//
// The third is the reason this exists at all. The corpus can only assert what
// someone thought to assert; a state can satisfy every invariant on the list
// and still be visibly wrong — the stage 3 branch flip was exactly that, and
// no assertion caught it. Watching is a different instrument from checking, and
// it needs the same session to be replayable rather than re-performed.
//
// Format properties, all tested, and the same four persist commits to:
//
//   versioned      from the first write. Churn before the stage 8 freeze
//                  regenerates scripts rather than growing migration shims.
//   lossless       screen coordinates round-trip exactly. A script that
//                  replays 0.5 px off is a script that stops reproducing the
//                  hit it was recorded to demonstrate.
//   self-contained the starting document is embedded, not referenced, so a
//                  script keeps working when the fixture that made it moves.
//   deterministic  replay is a pure function of the file and the feel policy in
//                  force. Recording a replay reproduces the file it came from,
//                  which is the round-trip property the corpus pins.
//
// The policy is deliberately *not* recorded. A script replays under whatever
// hit and snap policy the build currently has, so changing a feel number shows
// up as a corpus diff to be reviewed — which is the stated discipline, that
// changing a policy means updating the corpus deliberately rather than
// silently. A script that froze its own policy would be a script that could
// never tell you a policy change had broken it.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/document.h"
#include "interact/drag.h"
#include "interact/events.h"
#include "interact/tools.h"

namespace paroculus {

class Session;
enum class Key : uint8_t;

// One recorded step. A flat struct rather than a variant because the format is
// line-per-step and a parser reading one line fills one of these; the kind
// says which fields carry meaning.
struct ScriptStep {
    enum class Kind : uint8_t {
        Viewport, Pointer, Key, Tool, Confirm, Decline,
        // Typed entry. One character per step rather than a whole field,
        // because a half-typed value is a state the user was really in and a
        // replay that skipped to the finished string would skip the feel.
        Type, NumericResolve, NumericImpose, NumericBackspace, NumericAdvance,
        NumericCancel,
    };
    Kind kind = Kind::Pointer;

    // Kind::Confirm and Kind::Decline. An index into the offered candidates or
    // into the constraints the last placement declared — both are rank-ordered
    // lists the user was looking at, so the index is what they actually chose.
    size_t index = 0;

    // Kind::Type.
    char character = 0;

    // Kind::Tool. Without this a drawing session could not be recorded at all:
    // the same click means "select this" or "place a point here" depending on
    // which tool is in force, so the tool changes are part of the input.
    ToolKind tool = ToolKind::Select;

    // Kind::Viewport. The view in force from this step onward, so a script that
    // pans or zooms mid-session replays through the same transforms it was
    // recorded under — without which every later screen coordinate would land
    // somewhere else.
    Viewport viewport;

    // Kind::Pointer. Only the screen position is stored: the document position
    // is derived through the current viewport at replay, exactly as
    // PointerEvent::at derives it at record time, so the two spaces cannot
    // drift apart in a file the way they could if both were written down.
    PointerAction action = PointerAction::Move;
    Button button = Button::None;
    Eigen::Vector2d screen = Eigen::Vector2d::Zero();

    // Kind::Key.
    uint8_t key = 0;

    Modifier modifiers = Modifier::None;
};

struct GestureScript {
    // The document as it stood when recording began. Embedded rather than
    // referenced: a script is a recording, and a recording that depends on
    // external state is a recording of only part of what happened.
    Document document;
    std::vector<ScriptStep> steps;
};

inline constexpr int SCRIPT_VERSION = 0;

// Returns the script as text. Deterministic: the same script serializes
// byte-identically on every machine and every run.
std::string serializeScript(const GestureScript &script);

struct ScriptLoadResult {
    bool ok = false;
    std::string error;
    size_t line = 0;  // 1-based, 0 when the failure is not line-specific

    explicit operator bool() const { return ok; }
};

// Replaces `out` wholesale. On failure `out` is left empty rather than
// half-populated, for the reason persist gives: a partially loaded artefact is
// a corrupt one wearing a valid one's interface.
ScriptLoadResult parseScript(std::string_view text, GestureScript &out);

// Feeds one step to a session. Public because playback is stepwise — the whole
// point of watching is to watch it happen, not to see the result.
void applyStep(Session &session, const ScriptStep &step);

// Feeds every step, in order.
void replay(Session &session, const GestureScript &script);

// Captures what a session was asked to do.
//
// Recording lives here rather than in Session because a recorder is not part of
// interacting: Session calls into one when it has been given one, and knows
// nothing else about it.
class ScriptRecorder {
public:
    void viewport(const Viewport &viewport);
    void pointer(const PointerEvent &event);
    void key(Key key, Modifier modifiers);
    void tool(ToolKind kind);
    void confirm(size_t index);
    void decline(size_t index);
    void type(char c);
    void numeric(ScriptStep::Kind kind);

    const std::vector<ScriptStep> &steps() const { return steps_; }
    void clear() { steps_.clear(); }

private:
    std::vector<ScriptStep> steps_;
};

}  // namespace paroculus
