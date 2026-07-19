#include "interact/tools.h"

#include <algorithm>
#include <cmath>
#include <optional>

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
        case ToolKind::Select:    return "select";
        case ToolKind::Line:      return "line";
        case ToolKind::Circle:    return "circle";
        case ToolKind::Arc:       return "arc";
        case ToolKind::Rectangle: return "rectangle";
    }
    return "select";
}

ToolKind toolFromName(std::string_view name) {
    if(name == "line") return ToolKind::Line;
    if(name == "circle") return ToolKind::Circle;
    if(name == "arc") return ToolKind::Arc;
    if(name == "rectangle") return ToolKind::Rectangle;
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
        // The click that opened the chain created nothing at the time. This is
        // the point it turned out to mean, and the relations it captured bind
        // here. Continuing a chain opens nothing, so the list stays empty.
        out.opened.push_back(start);
    }
    const EntityId end = claim();
    out.commands.push_back(AddRecord<EntityRecord>{pointRecord(end, cursor)});
    const EntityId segment = claim();
    out.commands.push_back(AddRecord<EntityRecord>{segmentRecord(segment, start, end)});
    out.placed.point = end;
    out.placed.segment = segment;
    lastStart_ = start;
    lastEnd_ = end;
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
    // A point at the far end and the segment reaching it. The band is a real
    // segment, so the direction-valued kinds have something to be about.
    p.willPlace = PlacementRoles{true, true, false};
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


bool LineTool::setParameter(size_t index, double value) {
    if(!anchored_ || !haveCursor_) return false;
    const double dx = cursor_.x - anchor_.x;
    const double dy = cursor_.y - anchor_.y;
    const double length = std::hypot(dx, dy);

    if(index == 0) {
        if(value <= 0.0) return false;
        // Along the direction the hand chose, at the length the digits chose.
        // With no direction yet, a typed length has nowhere to point.
        if(length == 0.0) return false;
        cursor_ = Point{anchor_.x + dx / length * value, anchor_.y + dy / length * value};
        refreshParameters();
        return true;
    }
    if(index == 1) {
        // Keep the length the hand chose and take the angle from the digits.
        const double radians = value * 3.14159265358979323846 / 180.0;
        const double keep = length > 0.0 ? length : 0.0;
        if(keep == 0.0) return false;
        cursor_ = Point{anchor_.x + std::cos(radians) * keep,
                        anchor_.y + std::sin(radians) * keep};
        refreshParameters();
        return true;
    }
    return false;
}

std::optional<ConstraintRecord> LineTool::dimensionFor(size_t index, double value,
                                                       const ToolOutput &out) const {
    // Length pins as a distance between the endpoints the step created. Angle
    // has no dimension here: an angle constraint is an angle *to* something,
    // and this gesture has drawn only one segment.
    if(index != 0) return std::nullopt;
    (void)out;
    if(!lastStart_.valid() || !lastEnd_.valid()) return std::nullopt;

    ConstraintRecord r;
    r.kind = ConstraintKind::PointPointDistance;
    r.operands[0] = lastStart_;
    r.operands[1] = lastEnd_;
    r.value = Slot(value);
    return r;
}

// ---------------------------------------------------------------------------
// Circle
// ---------------------------------------------------------------------------

ToolOutput CircleTool::press(const Document &doc, Point cursor) {
    cursor_ = cursor;
    haveCursor_ = true;
    if(!haveCentre_) {
        haveCentre_ = true;
        centre_ = cursor;
        parameters_[0].value = 0.0;
        return {};
    }

    const double radius = std::hypot(cursor.x - centre_.x, cursor.y - centre_.y);
    // A zero-radius circle is not a circle, and the solver would be asked to
    // hold a degenerate one forever. Treat it as a slip and keep the centre.
    if(radius <= 0.0) return {};

    uint32_t next = doc.entities().allocator().next();
    auto claim = [&next]() { return EntityId(next++); };

    ToolOutput out;
    out.label = "circle";
    const EntityId centre = claim();
    out.commands.push_back(AddRecord<EntityRecord>{pointRecord(centre, centre_)});

    EntityRecord circle;
    circle.id = claim();
    circle.kind = EntityKind::Circle;
    circle.points = {centre, EntityId(), EntityId()};
    // The radius is the circle's own parameter, seeded here and owned by the
    // solver afterwards.
    circle.seeds = {radius, 0.0};
    out.commands.push_back(AddRecord<EntityRecord>{circle});

    // The opening click placed the centre, and its relations bind there.
    out.opened.push_back(centre);
    // The committing click placed no point at all: the rim is a radius, and
    // there is no entity sitting on it. Naming the centre as the placed point
    // would be the plausible wrong answer — a rim snap would then declare the
    // centre coincident with whatever the rim touched, and the circle would
    // teleport. What a rim snap means is that the circle passes through, which
    // is a relation about the curve.
    out.placed.curve = circle.id;
    lastCircle_ = circle.id;
    return out;
}

void CircleTool::move(const Document &doc, Point cursor) {
    (void)doc;
    cursor_ = cursor;
    haveCursor_ = true;
    parameters_[0].value =
        haveCentre_ ? std::hypot(cursor.x - centre_.x, cursor.y - centre_.y) : 0.0;
}

bool CircleTool::escape() {
    if(!haveCentre_) return false;
    haveCentre_ = false;
    parameters_[0].value = 0.0;
    return true;
}

void CircleTool::committed() {
    // Each circle is its own gesture: no chaining, so the tool rearms empty.
    haveCentre_ = false;
    parameters_[0].value = 0.0;
}

ToolPreview CircleTool::preview() const {
    ToolPreview p;
    p.active = haveCentre_ && haveCursor_;
    p.from = centre_;
    p.to = cursor_;
    // The band is a radius, not a segment: nothing is being drawn from the
    // centre to the rim, so there is nothing for horizontal or parallel to
    // describe and they are not generated.
    p.willPlace = PlacementRoles{false, false, true};
    return p;
}


bool CircleTool::setParameter(size_t index, double value) {
    if(index != 0 || !haveCentre_ || value <= 0.0) return false;
    const double dx = cursor_.x - centre_.x;
    const double dy = cursor_.y - centre_.y;
    const double length = std::hypot(dx, dy);
    // Direction from the hand where there is one, and due east when the cursor
    // is still sitting on the centre, so a typed radius always resolves.
    const double ux = length > 0.0 ? dx / length : 1.0;
    const double uy = length > 0.0 ? dy / length : 0.0;
    cursor_ = Point{centre_.x + ux * value, centre_.y + uy * value};
    haveCursor_ = true;
    parameters_[0].value = value;
    return true;
}

std::optional<ConstraintRecord> CircleTool::dimensionFor(size_t index, double value,
                                                         const ToolOutput &out) const {
    (void)out;
    if(index != 0 || !lastCircle_.valid()) return std::nullopt;
    ConstraintRecord r;
    r.kind = ConstraintKind::Radius;
    r.operands[0] = lastCircle_;
    r.value = Slot(value);
    return r;
}

// ---------------------------------------------------------------------------
// Arc
// ---------------------------------------------------------------------------

namespace {

// The circle through three points, or nullopt when they are collinear — which
// is not a failure, it is the user drawing something an arc cannot be.
std::optional<Point> circumcentre(Point a, Point b, Point c) {
    const double d =
        2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if(std::abs(d) < 1e-12) return std::nullopt;
    const double a2 = a.x * a.x + a.y * a.y;
    const double b2 = b.x * b.x + b.y * b.y;
    const double c2 = c.x * c.x + c.y * c.y;
    return Point{(a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
                 (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d};
}

constexpr double TAU = 6.283185307179586476925;

double normalised(double angle) {
    while(angle < 0.0) angle += TAU;
    while(angle >= TAU) angle -= TAU;
    return angle;
}

}  // namespace

std::optional<ArcTool::Ghost> ArcTool::ghost() const {
    if(clicks_ < 2 || !haveCursor_) return std::nullopt;
    const std::optional<Point> centre = circumcentre(start_, cursor_, end_);
    if(!centre) return std::nullopt;

    Ghost g;
    g.centre = *centre;
    g.radius = std::hypot(start_.x - centre->x, start_.y - centre->y);
    const double startAngle = std::atan2(start_.y - centre->y, start_.x - centre->x);
    const double endAngle = std::atan2(end_.y - centre->y, end_.x - centre->x);
    const double throughAngle = std::atan2(cursor_.y - centre->y, cursor_.x - centre->x);

    // Which way round depends on which side the bulge went, so the arc follows
    // the hand rather than a convention the user has to learn.
    const double forward = normalised(endAngle - startAngle);
    const double toThrough = normalised(throughAngle - startAngle);
    if(toThrough <= forward) {
        g.startAngle = startAngle;
        g.sweep = forward;
        g.reversed = false;
    } else {
        g.startAngle = endAngle;
        g.sweep = TAU - forward;
        g.reversed = true;
    }
    return g;
}

ToolOutput ArcTool::press(const Document &doc, Point cursor) {
    cursor_ = cursor;
    haveCursor_ = true;

    if(clicks_ == 0) {
        start_ = cursor;
        clicks_ = 1;
        refreshParameters();
        return {};
    }
    if(clicks_ == 1) {
        end_ = cursor;
        clicks_ = 2;
        refreshParameters();
        return {};
    }

    const std::optional<Ghost> g = ghost();
    // Three collinear clicks describe no arc. Keep what was placed rather than
    // committing something degenerate, so the user can move and click again.
    if(!g || g->radius <= 0.0) return {};

    uint32_t next = doc.entities().allocator().next();
    auto claim = [&next]() { return EntityId(next++); };
    uint32_t nextConstraint = doc.constraints().allocator().next();

    ToolOutput out;
    out.label = "arc";

    // The centre is construction: real geometry, selectable and constrainable,
    // but not part of the drawn shape — and excluded from snapping by role, so
    // an arc does not leave a magnet behind that nobody aimed at.
    EntityRecord centre = pointRecord(claim(), g->centre);
    centre.role = Role::Construction;
    out.commands.push_back(AddRecord<EntityRecord>{centre});

    // Endpoints in the solver's order: counter-clockwise from start to end.
    const Point from{g->centre.x + g->radius * std::cos(g->startAngle),
                     g->centre.y + g->radius * std::sin(g->startAngle)};
    const Point to{g->centre.x + g->radius * std::cos(g->startAngle + g->sweep),
                   g->centre.y + g->radius * std::sin(g->startAngle + g->sweep)};
    const EntityId startPoint = claim();
    const EntityId endPoint = claim();
    out.commands.push_back(AddRecord<EntityRecord>{pointRecord(startPoint, from)});
    out.commands.push_back(AddRecord<EntityRecord>{pointRecord(endPoint, to)});

    EntityRecord arc;
    arc.id = claim();
    arc.kind = EntityKind::Arc;
    arc.points = {centre.id, startPoint, endPoint};
    out.commands.push_back(AddRecord<EntityRecord>{arc});

    // The through point the user actually aimed at, held on the arc. Without it
    // the bulge would be a one-off choice rather than a declared relation, and
    // dragging the endpoints would lose the shape the user drew.
    const EntityId through = claim();
    const Point onArc{g->centre.x + g->radius * std::cos(std::atan2(cursor_.y - g->centre.y,
                                                                    cursor_.x - g->centre.x)),
                      g->centre.y + g->radius * std::sin(std::atan2(cursor_.y - g->centre.y,
                                                                    cursor_.x - g->centre.x))};
    out.commands.push_back(AddRecord<EntityRecord>{pointRecord(through, onArc)});

    ConstraintRecord onCircle;
    // Claimed, not left null for the document to allocate. Session claims its
    // inferred relations past whatever the tool already took, and it can only
    // see what a record names — a null id reads as nothing taken, so the first
    // inferred coincidence would be handed this record's id and the whole step
    // would be refused for the collision. All-or-nothing means that costs the
    // arc, not one relation.
    onCircle.id = ConstraintId(nextConstraint++);
    onCircle.kind = ConstraintKind::PointOnCircle;
    onCircle.operands[0] = through;
    onCircle.operands[1] = arc.id;
    out.commands.push_back(AddRecord<ConstraintRecord>{onCircle});

    // Both opening clicks bound relations, and both bind now — in click order,
    // which is not the arc's own order when the bulge reversed it. Binding the
    // first click's snaps to the arc's start point regardless would silently
    // attach the user's coincidence to the wrong end of half the arcs they
    // draw, and every asserted invariant would still hold.
    out.opened.push_back(g->reversed ? endPoint : startPoint);
    out.opened.push_back(g->reversed ? startPoint : endPoint);

    // The third click did place a point: the through point is real geometry at
    // the cursor, so a snap there is an ordinary coincidence.
    out.placed.point = through;
    out.placed.curve = arc.id;
    return out;
}

void ArcTool::move(const Document &doc, Point cursor) {
    (void)doc;
    cursor_ = cursor;
    haveCursor_ = true;
    refreshParameters();
}

bool ArcTool::escape() {
    if(clicks_ == 0) return false;
    clicks_ = 0;
    refreshParameters();
    return true;
}

void ArcTool::committed() {
    clicks_ = 0;
    refreshParameters();
}

ToolPreview ArcTool::preview() const {
    ToolPreview p;
    // Between the first and second click the gesture is a chord, and a straight
    // rubber band is the honest preview of it.
    p.active = clicks_ >= 1 && haveCursor_;
    p.from = start_;
    p.to = clicks_ >= 2 ? end_ : cursor_;
    // A chord, then a bulge — never a segment. The through point is the only
    // point the commit places at the cursor.
    p.willPlace = PlacementRoles{true, false, true};
    if(const std::optional<Ghost> g = ghost()) {
        p.arcActive = true;
        p.arcCentre = g->centre;
        p.arcRadius = g->radius;
        p.arcStart = g->startAngle;
        p.arcSweep = g->sweep;
    }
    return p;
}

void ArcTool::refreshParameters() {
    const std::optional<Ghost> g = ghost();
    parameters_[0].value = g ? g->radius : 0.0;
    parameters_[1].value = g ? g->sweep * 180.0 / 3.14159265358979323846 : 0.0;
}

// ---------------------------------------------------------------------------
// Rectangle
// ---------------------------------------------------------------------------

ToolOutput RectangleTool::press(const Document &doc, Point cursor) {
    cursor_ = cursor;
    haveCursor_ = true;
    if(!haveCorner_) {
        haveCorner_ = true;
        corner_ = cursor;
        parameters_[0].value = 0.0;
        parameters_[1].value = 0.0;
        return {};
    }

    const double width = cursor.x - corner_.x;
    const double height = cursor.y - corner_.y;
    // A rectangle with no extent in either axis is a degenerate one the solver
    // would be asked to hold. Keep the corner and let the user try again.
    if(width == 0.0 || height == 0.0) return {};

    uint32_t nextEntity = doc.entities().allocator().next();
    uint32_t nextConstraint = doc.constraints().allocator().next();
    auto claimEntity = [&nextEntity]() { return EntityId(nextEntity++); };
    auto claimConstraint = [&nextConstraint]() { return ConstraintId(nextConstraint++); };

    ToolOutput out;
    out.label = "rectangle";

    const Point corners[4] = {corner_,
                              Point{cursor.x, corner_.y},
                              cursor,
                              Point{corner_.x, cursor.y}};

    // Each edge gets its own endpoints, joined to its neighbours by
    // coincidence. Shared points would be simpler and would be wrong: a corner
    // has to be openable by deleting one relation, which is what "dissolves
    // gracefully, leaving perfectly ordinary constrained geometry" means.
    EntityId ends[4][2];
    EntityId edges[4];
    for(int i = 0; i < 4; i++) {
        const Point a = corners[i];
        const Point b = corners[(i + 1) % 4];
        ends[i][0] = claimEntity();
        out.commands.push_back(AddRecord<EntityRecord>{pointRecord(ends[i][0], a)});
        ends[i][1] = claimEntity();
        out.commands.push_back(AddRecord<EntityRecord>{pointRecord(ends[i][1], b)});
        edges[i] = claimEntity();
        out.commands.push_back(
            AddRecord<EntityRecord>{segmentRecord(edges[i], ends[i][0], ends[i][1])});
    }

    for(int i = 0; i < 4; i++) {
        ConstraintRecord join;
        join.id = claimConstraint();
        join.kind = ConstraintKind::Coincident;
        join.operands[0] = ends[i][1];
        join.operands[1] = ends[(i + 1) % 4][0];
        out.commands.push_back(AddRecord<ConstraintRecord>{join});
    }

    // Horizontal on the pair that runs across, vertical on the pair that runs
    // up. Four constraints over eight free parameters leaves four degrees of
    // freedom — position, width and height — which is what a rectangle is.
    for(int i = 0; i < 4; i++) {
        ConstraintRecord axis;
        axis.id = claimConstraint();
        axis.kind = (i % 2 == 0) ? ConstraintKind::Horizontal : ConstraintKind::Vertical;
        axis.operands[0] = edges[i];
        out.commands.push_back(AddRecord<ConstraintRecord>{axis});
    }

    // Inference binds to the corner the user placed first, so starting a
    // rectangle on an existing vertex means what it looks like it means.
    out.opened.push_back(ends[0][0]);
    // And to the corner the closing click placed — corners[2], which edge 1
    // ends at. Without this the closing snap would correct the position and
    // declare nothing: the rectangle would sit on the vertex it landed on
    // rather than being bound to it, and the first drag would peel them apart.
    out.placed.point = ends[1][1];
    lastWidth_[0] = ends[0][0];
    lastWidth_[1] = ends[0][1];
    lastHeight_[0] = ends[1][0];
    lastHeight_[1] = ends[1][1];
    return out;
}


bool RectangleTool::setParameter(size_t index, double value) {
    if(!haveCorner_ || index > 1 || value <= 0.0) return false;
    // Sign from the hand, magnitude from the digits: typing a width must not
    // flip a rectangle the user has drawn to the left.
    const double sx = cursor_.x >= corner_.x ? 1.0 : -1.0;
    const double sy = cursor_.y >= corner_.y ? 1.0 : -1.0;
    if(index == 0) cursor_.x = corner_.x + sx * value;
    if(index == 1) cursor_.y = corner_.y + sy * value;
    haveCursor_ = true;
    parameters_[0].value = std::abs(cursor_.x - corner_.x);
    parameters_[1].value = std::abs(cursor_.y - corner_.y);
    return true;
}

std::optional<ConstraintRecord> RectangleTool::dimensionFor(size_t index, double value,
                                                            const ToolOutput &out) const {
    (void)out;
    const EntityId *edge = index == 0 ? lastWidth_ : lastHeight_;
    if(index > 1 || !edge[0].valid() || !edge[1].valid()) return std::nullopt;
    ConstraintRecord r;
    r.kind = ConstraintKind::PointPointDistance;
    r.operands[0] = edge[0];
    r.operands[1] = edge[1];
    r.value = Slot(value);
    return r;
}

void RectangleTool::move(const Document &doc, Point cursor) {
    (void)doc;
    cursor_ = cursor;
    haveCursor_ = true;
    parameters_[0].value = haveCorner_ ? std::abs(cursor.x - corner_.x) : 0.0;
    parameters_[1].value = haveCorner_ ? std::abs(cursor.y - corner_.y) : 0.0;
}

bool RectangleTool::escape() {
    if(!haveCorner_) return false;
    haveCorner_ = false;
    parameters_[0].value = 0.0;
    parameters_[1].value = 0.0;
    return true;
}

void RectangleTool::committed() {
    haveCorner_ = false;
    parameters_[0].value = 0.0;
    parameters_[1].value = 0.0;
}

ToolPreview RectangleTool::preview() const {
    ToolPreview p;
    p.active = haveCorner_ && haveCursor_;
    p.from = corner_;
    p.to = cursor_;
    // The band spans a diagonal, and a diagonal is not one of the four
    // segments this places. Offering to make it horizontal — or parallel to
    // something — would be offering a relation about a line that never exists.
    // The edges get their axis constraints from the macro itself.
    p.willPlace = PlacementRoles{true, false, false};
    return p;
}

}  // namespace paroculus
