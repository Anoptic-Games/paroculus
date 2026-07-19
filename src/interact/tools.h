// Creation tools.
//
// Verb-noun by necessity — the noun does not exist yet — and deliberately
// shallow: no nested modes, live parameters in a fixed strip, Esc out. That
// shallowness is a commitment rather than an omission. Every mode a tool can be
// in is a mode the user has to track, and the tool that needs a submode is the
// tool that has stopped being one tool.
//
// Selection is the home state and Esc always lands there. Inside a tool Esc
// ends what is in flight; pressed again with nothing in flight it leaves the
// tool. Two presses to get home from anywhere, and never more.
//
// Tools never touch the document. They return commands and the session applies
// them through the journal, because Session is the only writer and every
// visible change has to be undoable. A tool that mutated directly would be a
// tool whose work could not be undone.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/pose.h"

namespace paroculus {

enum class ToolKind : uint8_t { Select, Line, Circle, Arc, Rectangle };

// Returns a stable lowercase label. Scripts and the registry both name tools by
// it, so it is format, not presentation.
const char *toolName(ToolKind kind);
// Returns Select for anything unrecognised.
ToolKind toolFromName(std::string_view name);

// A live tool parameter for the fixed strip: data, never a widget. Stage 4's
// numeric entry types into exactly these, which is why the value is carried
// separately from any formatting of it.
struct ToolParameter {
    const char *name = "";
    double value = 0.0;
};

// What the tool wants drawn while it runs.
//
// The rubber band is a preview, and preview shows truth: once the snap engine
// lands, `to` is the corrected placement rather than the raw cursor, so what is
// on screen mid-gesture is what commit will produce.
struct ToolPreview {
    bool active = false;
    Point from;
    Point to;
    // The entity the band starts from, once the chain hangs off a real point.
    // Inference reads it to keep a placement from snapping to its own anchor.
    EntityId fromEntity;

    // A curve gesture describes an arc rather than a chord, and the preview has
    // to show the shape that will land rather than the one the two clicks
    // happen to span.
    bool arcActive = false;
    Point arcCentre;
    double arcRadius = 0.0;
    double arcStart = 0.0;
    double arcSweep = 0.0;
};

// What a tool asks the session to do. Empty when the event changed nothing.
struct ToolOutput {
    std::string label;
    std::vector<Command> commands;

    // What inference may bind to, named by the tool because only the tool knows
    // which of the ids it claimed are the placement. A macro tool declares its
    // own subjects the same way.
    //
    // placedStart is valid only on the step that opens a chain, because that is
    // the only step that creates the point the chain starts from — and the
    // relations inferred for it were captured a click earlier, when the user
    // was pointing at it.
    EntityId placedStart;
    EntityId placedPoint;
    EntityId placedSegment;
};

class Tool {
public:
    virtual ~Tool() = default;

    virtual ToolKind kind() const = 0;

    // cursor: the placement position in document units. Snapping happens before
    // this call, so a tool never sees a raw cursor and never has to know that
    // snapping exists.
    virtual ToolOutput press(const Document &doc, Point cursor) = 0;
    virtual void move(const Document &doc, Point cursor) = 0;

    // Returns true when Esc was consumed because something was in flight.
    // False means the tool is idle and the session should return to selection.
    virtual bool escape() = 0;

    // Called only when the session actually applied this tool's commands, so a
    // refused step leaves the tool where it was rather than chaining off
    // geometry that does not exist.
    virtual void committed() = 0;

    virtual ToolPreview preview() const = 0;
    virtual std::span<const ToolParameter> parameters() const = 0;

    // Resolves the placement in flight so parameter `index` takes `value`
    // exactly. Returns false when the tool cannot honour it — a length of zero,
    // or a parameter that has no geometric meaning yet.
    //
    // Exactly, not nearly: this is the entrance that exists because dragging
    // cannot hit a number, so landing near it would defeat the point.
    virtual bool setParameter(size_t index, double value) { (void)index; (void)value; return false; }

    // The dimension that would pin parameter `index` at `value`, expressed
    // against the entities this step created. Absent when the parameter has no
    // constraint that says it — an angle needs a second segment to be an angle
    // to, and the tool has only drawn one.
    virtual std::optional<ConstraintRecord> dimensionFor(size_t index, double value,
                                                         const ToolOutput &out) const {
        (void)index;
        (void)value;
        (void)out;
        return std::nullopt;
    }
};

// Chained segments: click to anchor, click to complete, and the completed end
// anchors the next one. Esc ends the chain.
//
// Nothing is committed until a segment exists. A first click that is then
// abandoned leaves no stray point behind, and each committed segment is exactly
// one undo step — which is the granularity the journal documents as one user
// gesture.
class LineTool : public Tool {
public:
    ToolKind kind() const override { return ToolKind::Line; }

    ToolOutput press(const Document &doc, Point cursor) override;
    void move(const Document &doc, Point cursor) override;
    bool escape() override;
    void committed() override;

    ToolPreview preview() const override;
    std::span<const ToolParameter> parameters() const override { return parameters_; }
    bool setParameter(size_t index, double value) override;
    std::optional<ConstraintRecord> dimensionFor(size_t index, double value,
                                                 const ToolOutput &out) const override;

private:
    void refreshParameters();

    // The anchor, in one of two forms. Before the first segment commits there is
    // no entity to point at, only a position; afterwards the chain hangs off a
    // real point so the segments share endpoints rather than merely touching.
    bool anchored_ = false;
    Point anchor_;
    EntityId anchorEntity_;
    // The endpoint the pending step will create, promoted to the anchor only
    // once that step is known to have applied.
    EntityId pendingEnd_;
    // What the last press claimed, so a dimension can name it without
    // reconstructing the emission order by arithmetic.
    EntityId lastStart_;
    EntityId lastEnd_;

    Point cursor_;
    bool haveCursor_ = false;

    std::array<ToolParameter, 2> parameters_{ToolParameter{"length", 0.0},
                                             ToolParameter{"angle", 0.0}};
};

// Centre then rim. One segment of the gesture sets the centre, the next sets
// the radius, and the circle owns that radius as its own parameter — which is
// why a circle needs no second point in the document to remember how big it is.
class CircleTool : public Tool {
public:
    ToolKind kind() const override { return ToolKind::Circle; }

    ToolOutput press(const Document &doc, Point cursor) override;
    void move(const Document &doc, Point cursor) override;
    bool escape() override;
    void committed() override;

    ToolPreview preview() const override;
    std::span<const ToolParameter> parameters() const override { return parameters_; }
    bool setParameter(size_t index, double value) override;
    std::optional<ConstraintRecord> dimensionFor(size_t index, double value,
                                                 const ToolOutput &out) const override;

private:
    bool haveCentre_ = false;
    Point centre_;
    Point cursor_;
    bool haveCursor_ = false;
    EntityId lastCircle_;
    std::array<ToolParameter, 1> parameters_{ToolParameter{"radius", 0.0}};
};

// Start, then end, then bulge — and what lands is a macro, not an arc type.
//
// The document stores a centre-form arc, because that is what the solver
// solves. The centre is a construction point: it is real geometry the user can
// select and constrain, but it is not part of the drawn shape and it is
// excluded from snapping by role, so an arc does not litter the sketch with a
// magnet nobody aimed at.
//
// There is no convert-to-path cliff here because there is nothing to convert:
// an arc is a solver arc and three ordinary points from the moment it exists.
class ArcTool : public Tool {
public:
    ToolKind kind() const override { return ToolKind::Arc; }

    ToolOutput press(const Document &doc, Point cursor) override;
    void move(const Document &doc, Point cursor) override;
    bool escape() override;
    void committed() override;

    ToolPreview preview() const override;
    std::span<const ToolParameter> parameters() const override { return parameters_; }

    // The arc currently being previewed, for the ghost. Absent until there is
    // enough of a gesture to define one.
    struct Ghost {
        Point centre;
        double radius = 0.0;
        double startAngle = 0.0;
        double sweep = 0.0;
    };
    std::optional<Ghost> ghost() const;

private:
    void refreshParameters();

    int clicks_ = 0;
    Point start_;
    Point end_;
    Point cursor_;
    bool haveCursor_ = false;
    std::array<ToolParameter, 2> parameters_{ToolParameter{"radius", 0.0},
                                             ToolParameter{"sweep", 0.0}};
};

// A rectangle is not a type. It is four segments, four coincidences and four
// axis constraints, and the tag that would give it corner handles is a stage 7
// concern that owns nothing and can arrive later without a model change.
//
// The corners are separate points joined by coincidences rather than shared
// points, deliberately: that is what lets a corner be opened by deleting one
// relation, which is the graceful dissolution the macro design promises. Shared
// points cannot be un-shared without rebuilding the geometry.
class RectangleTool : public Tool {
public:
    ToolKind kind() const override { return ToolKind::Rectangle; }

    ToolOutput press(const Document &doc, Point cursor) override;
    void move(const Document &doc, Point cursor) override;
    bool escape() override;
    void committed() override;

    ToolPreview preview() const override;
    std::span<const ToolParameter> parameters() const override { return parameters_; }
    bool setParameter(size_t index, double value) override;
    std::optional<ConstraintRecord> dimensionFor(size_t index, double value,
                                                 const ToolOutput &out) const override;

    bool spanning() const { return haveCorner_ && haveCursor_; }
    Point corner() const { return corner_; }
    Point opposite() const { return cursor_; }

private:
    bool haveCorner_ = false;
    Point corner_;
    Point cursor_;
    bool haveCursor_ = false;
    // The endpoints of the width edge and the height edge, from the last press.
    EntityId lastWidth_[2];
    EntityId lastHeight_[2];
    std::array<ToolParameter, 2> parameters_{ToolParameter{"width", 0.0},
                                             ToolParameter{"height", 0.0}};
};

}  // namespace paroculus
