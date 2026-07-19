#include "interact/tools.h"

#include <cmath>

namespace paroculus {
namespace {

EntityRecord pointRecord(EntityId id, Point p) {
    EntityRecord r;
    r.id = id;
    r.kind = EntityKind::Point;
    r.seeds = {p.x, p.y};
    return r;
}

EntityRecord segmentRecord(EntityId id, EntityId a, EntityId b) {
    EntityRecord r;
    r.id = id;
    r.kind = EntityKind::Segment;
    r.points = {a, b, EntityId()};
    return r;
}

}  // namespace

const char *toolName(ToolKind kind) {
    switch(kind) {
        case ToolKind::Select: return "select";
        case ToolKind::Line:   return "line";
    }
    return "select";
}

ToolKind toolFromName(std::string_view name) {
    if(name == "line") return ToolKind::Line;
    return ToolKind::Select;
}

ToolOutput LineTool::press(const Document &doc, Point cursor) {
    cursor_ = cursor;
    haveCursor_ = true;

    // First click of a chain anchors a position and commits nothing. A click
    // the user then abandons has to leave the document exactly as it was, and
    // a lone floating point is not "exactly as it was".
    if(!anchored_) {
        anchored_ = true;
        anchor_ = cursor;
        anchorEntity_ = EntityId();
        refreshParameters();
        return {};
    }

    // The ids are assigned here rather than left for the document to allocate,
    // because the segment has to name its endpoints and validation refuses a
    // record whose operands do not exist yet. IDs are monotonic and never
    // reused, and the step applies atomically, so reading the watermark and
    // claiming the next few is exact rather than a guess.
    uint32_t next = doc.entities().allocator().next();
    auto claim = [&next]() { return EntityId(next++); };

    ToolOutput out;
    out.label = "line";
    // The anchor becomes a real point only now, when a segment will exist to
    // justify it. Continuing a chain reuses the point already in the document,
    // so consecutive segments share an endpoint rather than stacking two
    // coincident points the solver would then have to be told about.
    const EntityId start = anchorEntity_.valid() ? anchorEntity_ : claim();
    if(!anchorEntity_.valid()) {
        out.commands.push_back(AddRecord<EntityRecord>{pointRecord(start, anchor_)});
        out.placedStart = start;
    }
    const EntityId end = claim();
    out.commands.push_back(AddRecord<EntityRecord>{pointRecord(end, cursor)});
    const EntityId segment = claim();
    out.commands.push_back(AddRecord<EntityRecord>{segmentRecord(segment, start, end)});
    out.placedPoint = end;
    out.placedSegment = segment;
    pendingEnd_ = end;
    return out;
}

void LineTool::committed() {
    // Chain from the far end of what was just created, so the next segment
    // shares that point rather than starting a coincident second one.
    if(!pendingEnd_.valid()) return;
    anchorEntity_ = pendingEnd_;
    anchor_ = cursor_;
    anchored_ = true;
    pendingEnd_ = EntityId();
    refreshParameters();
}

void LineTool::move(const Document &doc, Point cursor) {
    (void)doc;
    cursor_ = cursor;
    haveCursor_ = true;
    refreshParameters();
}

bool LineTool::escape() {
    // Ends the chain but stays in the tool, so a second Esc leaves it. Drawing
    // several unconnected runs is one tool activation, not several.
    if(!anchored_) return false;
    anchored_ = false;
    anchorEntity_ = EntityId();
    refreshParameters();
    return true;
}

ToolPreview LineTool::preview() const {
    ToolPreview p;
    p.active = anchored_ && haveCursor_;
    p.from = anchor_;
    p.to = cursor_;
    p.fromEntity = anchorEntity_;
    return p;
}

void LineTool::refreshParameters() {
    if(!anchored_ || !haveCursor_) {
        parameters_[0].value = 0.0;
        parameters_[1].value = 0.0;
        return;
    }
    const double dx = cursor_.x - anchor_.x;
    const double dy = cursor_.y - anchor_.y;
    parameters_[0].value = std::hypot(dx, dy);
    // Degrees, measured the way the document is drawn rather than the way the
    // screen is: Y is up in document space.
    parameters_[1].value = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
}

}  // namespace paroculus
