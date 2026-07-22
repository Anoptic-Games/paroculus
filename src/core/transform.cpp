#include "core/transform.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "core/composition.h"

namespace paroculus {
namespace {

// The seed-bearing entities a transform writes, and the shape of what it writes
// into them. Everything else in the closure — segments, arcs — owns no
// parameters at all, so a transform reaches it entirely through its points.
bool ownsParameters(EntityKind kind) { return entityInfo(kind).ownParamCount > 0; }

// The moved set, closed downward over defining points, deduplicated, in ID
// order. Shared by every entry point because the closure is what "the
// selection" means to a transform and computing it twice invites two answers.
std::vector<EntityId> closure(const Document &doc, std::span<const EntityId> selection) {
    std::unordered_set<EntityId> seen;
    std::vector<EntityId> out;

    // Iterative rather than recursive: an arc's points are points, so the walk
    // is two levels deep today, but nothing in the model promises that and a
    // depth assumption is the kind that is discovered by a stack overflow.
    std::vector<EntityId> pending(selection.begin(), selection.end());
    while(!pending.empty()) {
        const EntityId id = pending.back();
        pending.pop_back();
        if(!id.valid() || !seen.insert(id).second) continue;
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr) continue;
        out.push_back(id);
        const size_t points = entityInfo(r->kind).pointCount;
        for(size_t i = 0; i < points; i++) pending.push_back(r->points[i]);
    }

    std::sort(out.begin(), out.end());
    return out;
}

bool contains(const std::vector<EntityId> &set, EntityId id) {
    return std::binary_search(set.begin(), set.end(), id);
}

// Whether every operand a constraint binds lies inside the moved set. The
// question both the retarget walk and the rescale walk ask, and the reason a
// dimension to the world outside resists rather than being rewritten.
bool whollyInside(const Document &doc, const ConstraintRecord &c,
                  const std::vector<EntityId> &moved) {
    const size_t bound = boundOperandCount(c);
    for(size_t i = 0; i < bound; i++) {
        if(!contains(moved, c.operands[i])) return false;
    }
    return bound > 0;
}

// Whether any parameter this transform would write is locked.
//
// Seeding a locked parameter is a move, not a request for one: the solver takes
// a fixed parameter's seed as its known value, so a transform that wrote through
// a lock would move locked geometry with nothing in the solve to stop it. That
// is the one thing a lock exists to prevent, so the transform refuses whole
// rather than moving what it can — a rotation that turned half a cluster is
// worse than one that reports it cannot turn this one.
bool anyLocked(const Document &doc, const std::vector<EntityId> &moved) {
    for(EntityId id : moved) {
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr || !ownsParameters(r->kind)) continue;
        if(isLocked(doc, id)) return true;
    }
    return false;
}

// Whether any frame-referenced relation binds an entity that would move.
//
// The kind means symmetry about the document frame through no operand, so it can
// be neither retargeted — there is no operand to point elsewhere — nor rewritten,
// since the frame is not the geometry moving. A transform about any centre but the
// world origin moves the geometry out from under it and the re-solve slides the
// pair back, which is the silent change the policy rules out. Refusing whole is
// the same rule a lock follows, and deleting the relation is how the user frees
// the cluster. Any binding entity counts, internal or straddling: either way the
// rewrite fights the world frame. In record (ID) order, so the verdict is stable.
bool anyFrameReferenced(const Document &doc, const std::vector<EntityId> &moved) {
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(!constraintInfo(c.kind).frameReferenced) continue;
        const size_t bound = boundOperandCount(c);
        for(size_t i = 0; i < bound; i++) {
            if(contains(moved, c.operands[i])) return true;
        }
    }
    return false;
}

// One entity's seeds rewritten by a map over positions and a factor over
// lengths. Returns nullopt when the entity owns nothing, so the caller emits no
// command rather than a set to an identical record.
template <typename PositionMap>
std::optional<EntityRecord> rewritten(const EntityRecord &r, const PositionMap &map,
                                      double lengthFactor) {
    if(!ownsParameters(r.kind)) return std::nullopt;
    EntityRecord out = r;
    if(r.kind == EntityKind::Point) {
        const Point p = map(Point{r.seeds[0], r.seeds[1]});
        out.seeds[0] = p.x;
        out.seeds[1] = p.y;
    } else if(r.kind == EntityKind::Circle) {
        // A circle's own parameter is its radius, and a radius is a length: it
        // rides the scale factor and is untouched by rotation, which is the
        // whole of what `lengthFactor` says.
        out.seeds[0] = r.seeds[0] * lengthFactor;
    }
    if(out == r) return std::nullopt;
    return out;
}

// Multiplies a slot by a factor.
//
// A constant folds, because a record has to compare equal to its own round-trip
// and a constant wrapped in a multiply node does not compare equal to the
// constant it evaluates to. An expression wraps, because a dimension driven by a
// named document parameter must stay driven by it — scaling a drawing is not a
// reason to sever the provenance the user built.
Slot scaledSlot(const Slot &slot, double factor) {
    if(slot.isConstant()) return Slot(slot.constant() * factor);
    return Slot::binary(ExprOp::Multiply, slot, Slot(factor));
}

// The extent of the moved points, for sizing a cluster frame. A frame an order
// of magnitude smaller than what it frames is a frame nobody can grab, and one
// of a fixed size is a guess about the document's scale.
double clusterExtent(const Document &doc, const std::vector<EntityId> &moved,
                     const Point &centre) {
    double furthest = 0.0;
    for(EntityId id : moved) {
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr || r->kind != EntityKind::Point) continue;
        const double dx = r->seeds[0] - centre.x;
        const double dy = r->seeds[1] - centre.y;
        furthest = std::max(furthest, std::hypot(dx, dy));
    }
    return furthest > 0.0 ? furthest : 1.0;
}

// Where a new record lands. A frame belongs with what it frames, so it takes the
// layer of the first moved entity rather than the base layer — hiding the
// cluster's layer hides its frame with it, which is what a user who put a
// cluster somewhere expects of the construction geometry that came with it.
LayerId frameLayer(const Document &doc, const std::vector<EntityId> &moved) {
    for(EntityId id : moved) {
        if(const EntityRecord *r = doc.entities().find(id)) return r->layer;
    }
    return LayerId();
}

}  // namespace

const char *transformErrorName(TransformError e) {
    switch(e) {
        case TransformError::None: return "none";
        case TransformError::NothingToMove: return "nothing-to-move";
        case TransformError::Degenerate: return "degenerate";
        case TransformError::NonUniform: return "non-uniform";
        case TransformError::Locked: return "locked";
        case TransformError::FrameReferenced: return "frame-referenced";
    }
    return "none";
}

std::vector<EntityId> transformClosure(const Document &doc,
                                       std::span<const EntityId> selection) {
    return closure(doc, selection);
}

std::vector<ConstraintId> axisReferencedIn(const Document &doc,
                                           std::span<const EntityId> moved) {
    const std::vector<EntityId> set(moved.begin(), moved.end());
    std::vector<ConstraintId> out;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind != ConstraintKind::Horizontal && c.kind != ConstraintKind::Vertical) continue;
        // Already retargeted: the user has said which frame this belongs to and
        // a rotation that moves both carries it along untouched. Reading the
        // bound count rather than the operand directly is what keeps "absent"
        // and "dangling" distinct here as everywhere else.
        if(boundOperandCount(c) > constraintInfo(c.kind).operandCount) continue;
        if(!contains(set, c.operands[0])) continue;
        out.push_back(c.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ConstraintId> absoluteValuedIn(const Document &doc,
                                           std::span<const EntityId> moved) {
    const std::vector<EntityId> set(moved.begin(), moved.end());
    std::vector<ConstraintId> out;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        const ConstraintKindInfo &info = constraintInfo(c.kind);
        if(info.invariance != Invariance::Absolute || info.valueArity == 0) continue;
        if(!whollyInside(doc, c, set)) continue;
        out.push_back(c.id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

// The body both rotate and scale share: rewrite every seed the closure reaches,
// then let the caller add whatever its own question answers.
//
// Split out because the two differ in exactly two things — the position map and
// the length factor — and a second copy of the closure, the lock check and the
// command emission is a second place for them to drift.
template <typename PositionMap>
TransformStep rewriteSeeds(const Document &doc, std::span<const EntityId> selection,
                           const PositionMap &map, double lengthFactor,
                           std::vector<EntityId> &moved) {
    TransformStep step;
    moved = closure(doc, selection);
    if(moved.empty()) {
        step.error = TransformError::NothingToMove;
        return step;
    }
    if(anyLocked(doc, moved)) {
        step.error = TransformError::Locked;
        return step;
    }
    // Before any command is emitted, so a refused transform leaves the document
    // byte-identical exactly as a lock refusal does.
    if(anyFrameReferenced(doc, moved)) {
        step.error = TransformError::FrameReferenced;
        return step;
    }

    bool writable = false;
    for(EntityId id : moved) {
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr) continue;
        if(ownsParameters(r->kind)) writable = true;
        if(std::optional<EntityRecord> next = rewritten(*r, map, lengthFactor)) {
            step.commands.push_back(SetRecord<EntityRecord>{*next});
            step.moved++;
        }
    }
    // A selection of nothing but segments is impossible — the closure pulls in
    // their endpoints — so reaching here means the selection named only records
    // that own no parameters, which is a transform with nowhere to write.
    if(!writable) step.error = TransformError::NothingToMove;
    return step;
}

}  // namespace

TransformStep rotateStep(const Document &doc, std::span<const EntityId> selection,
                         const RotateOptions &options) {
    const double c = std::cos(options.angle);
    const double s = std::sin(options.angle);
    const Point centre = options.centre;
    const auto map = [&](Point p) {
        const double dx = p.x - centre.x;
        const double dy = p.y - centre.y;
        return Point{centre.x + c * dx - s * dy, centre.y + s * dx + c * dy};
    };

    std::vector<EntityId> moved;
    TransformStep step = rewriteSeeds(doc, selection, map, 1.0, moved);
    if(!step.ok()) return step;
    if(options.axes != AxisAnswer::RetargetToClusterFrame) return step;

    const std::vector<ConstraintId> axes = axisReferencedIn(doc, moved);
    if(axes.empty()) return step;

    // One frame for the whole answer, not one per relation. A per-relation axis
    // would let two edges of the same rectangle end up parallel to two different
    // frames, which is a cluster that has stopped being rigid without anything
    // having said so.
    //
    // Construction geometry, so it constrains normally, never attracts a snap,
    // and stays out of regions and export. A per-cluster frame is just
    // construction geometry, which is the reason this costs no new machinery.
    uint32_t nextEntity = doc.entities().allocator().next();
    const EntityId origin(nextEntity++);
    const EntityId tip(nextEntity++);
    const EntityId axis(nextEntity++);
    const LayerId layer = frameLayer(doc, moved);
    const double length = clusterExtent(doc, moved, centre);

    EntityRecord originRecord;
    originRecord.id = origin;
    originRecord.kind = EntityKind::Point;
    originRecord.role = Role::Construction;
    originRecord.layer = layer;
    originRecord.seeds[0] = centre.x;
    originRecord.seeds[1] = centre.y;

    EntityRecord tipRecord = originRecord;
    tipRecord.id = tip;
    tipRecord.seeds[0] = centre.x + c * length;
    tipRecord.seeds[1] = centre.y + s * length;

    EntityRecord axisRecord;
    axisRecord.id = axis;
    axisRecord.kind = EntityKind::Segment;
    axisRecord.role = Role::Construction;
    axisRecord.layer = layer;
    axisRecord.points[0] = origin;
    axisRecord.points[1] = tip;

    // Before the retargets, because a constraint may not name an entity that is
    // not there yet — the same ordering rule the deletion cascade follows from
    // the other side.
    step.commands.push_back(AddRecord<EntityRecord>{originRecord});
    step.commands.push_back(AddRecord<EntityRecord>{tipRecord});
    step.commands.push_back(AddRecord<EntityRecord>{axisRecord});

    // Pinned, or the retarget destroys the rigidity it was chosen to preserve.
    //
    // A free frame is four new unknowns and no equations, so the cluster's
    // "horizontal" becomes parallel to a line nothing holds: a rectangle at dof
    // 4 comes out at dof 8 and the next corner drag tilts the whole thing. The
    // answer the user picked to keep the rectangle square would be the thing
    // that unsquared it.
    //
    // Pins rather than a lock, and this is the one place a pin is right where a
    // lock is not. A lock is presentation state and must never be able to make a
    // system inconsistent; these are relations the user asked for, on geometry
    // this step is creating, that nothing else constrains — so they cannot
    // conflict with anything, and deleting them is how a user frees the frame to
    // turn. That deletion is a feature: it is what "the subset's horizontal
    // tilts with it" looks like when the user wants it to keep tilting.
    uint32_t nextConstraint = doc.constraints().allocator().next();
    for(EntityId end : {origin, tip}) {
        ConstraintRecord pin;
        pin.id = ConstraintId(nextConstraint++);
        pin.kind = ConstraintKind::Pin;
        pin.operands[0] = end;
        step.commands.push_back(AddRecord<ConstraintRecord>{pin});
    }

    for(ConstraintId id : axes) {
        const ConstraintRecord *r = doc.constraints().find(id);
        if(r == nullptr) continue;
        ConstraintRecord next = *r;
        // The same declaration against a different reference: horizontal means
        // parallel to this axis and vertical means perpendicular to it, and the
        // difference is which solver primitive the translation picks. Nothing
        // about the relation the user declared has changed.
        next.operands[1] = axis;
        step.commands.push_back(SetRecord<ConstraintRecord>{next});
        step.retargeted++;
    }
    step.frame = axis;
    return step;
}

TransformStep scaleStep(const Document &doc, std::span<const EntityId> selection,
                        const ScaleOptions &options) {
    TransformStep step;
    // Strictly positive. Zero is a collapse, and a negative factor is a
    // reflection wearing a scale's clothes: it would leave circles with negative
    // radius seeds and, under scale-the-values, distance constraints demanding
    // negative lengths — a document no further edit recovers from. Reflection is
    // mirror's business, where it is a relation between the original and its
    // image rather than a sign flipped behind the user's back.
    if(!(options.factor > 0.0) || !std::isfinite(options.factor)) {
        step.error = TransformError::Degenerate;
        return step;
    }

    const Point centre = options.centre;
    const double f = options.factor;
    const auto map = [&](Point p) {
        return Point{centre.x + f * (p.x - centre.x), centre.y + f * (p.y - centre.y)};
    };

    std::vector<EntityId> moved;
    step = rewriteSeeds(doc, selection, map, f, moved);
    if(!step.ok()) return step;

    // Reported whichever answer was given, because a dimension reaching outside
    // the moved set resists either way and a scale that quietly failed to take
    // is the silent change the policy exists to rule out.
    for(const ConstraintRecord &c : doc.constraints().records()) {
        const ConstraintKindInfo &info = constraintInfo(c.kind);
        if(info.invariance != Invariance::Absolute || info.valueArity == 0) continue;
        if(whollyInside(doc, c, moved)) continue;
        const size_t bound = boundOperandCount(c);
        for(size_t i = 0; i < bound; i++) {
            if(contains(moved, c.operands[i])) {
                step.straddling++;
                break;
            }
        }
    }

    if(options.values != ValueAnswer::ScaleTheValues) return step;

    // Only the absolute family. Ratio, equal-length, angle and the rest are
    // scale-invariant by the taxonomy's own column, so rewriting them would be
    // asserting a change the scale did not make — that asymmetry is the thesis,
    // and it is read from the table rather than restated here.
    for(ConstraintId id : absoluteValuedIn(doc, moved)) {
        const ConstraintRecord *r = doc.constraints().find(id);
        if(r == nullptr) continue;
        ConstraintRecord next = *r;
        next.value = scaledSlot(r->value, f);
        step.commands.push_back(SetRecord<ConstraintRecord>{next});
        step.rescaled++;
    }
    return step;
}

TransformStep retargetAxesStep(const Document &doc, std::span<const EntityId> selection,
                               const RetargetOptions &options) {
    // A new cluster frame is a zero-degree rotation that retargets: delegating
    // rather than duplicating is what guarantees the parity the test asserts —
    // the rewrite is one function, reached two ways.
    if(options.target == RetargetTarget::NewClusterFrame) {
        RotateOptions ro;
        ro.centre = options.centre;
        ro.angle = 0.0;
        ro.axes = AxisAnswer::RetargetToClusterFrame;
        return rotateStep(doc, selection, ro);
    }

    TransformStep step;
    const std::vector<EntityId> moved = closure(doc, selection);
    if(moved.empty()) {
        step.error = TransformError::NothingToMove;
        return step;
    }
    // The same gates rotate refuses on, byte-identically: a lock or a
    // frame-referenced relation in the closure refuses the whole retarget.
    if(anyLocked(doc, moved)) {
        step.error = TransformError::Locked;
        return step;
    }
    if(anyFrameReferenced(doc, moved)) {
        step.error = TransformError::FrameReferenced;
        return step;
    }

    const EntityId newRef =
        options.target == RetargetTarget::ExistingFrame ? options.frame : EntityId();

    // Every axis relation whose required segment is inside the moved set, at any
    // current reference — so a retarget to the document frame reaches the ones a
    // cluster frame already holds, which axisReferencedIn deliberately skips.
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind != ConstraintKind::Horizontal && c.kind != ConstraintKind::Vertical) continue;
        if(!contains(moved, c.operands[0])) continue;
        const EntityId currentRef =
            boundOperandCount(c) > constraintInfo(c.kind).operandCount ? c.operands[1] : EntityId();
        if(currentRef == newRef) continue;  // already where asked; emit nothing
        ConstraintRecord next = c;
        next.operands[1] = newRef;
        step.commands.push_back(SetRecord<ConstraintRecord>{next});
        step.retargeted++;
    }
    step.frame = newRef;
    return step;
}

TransformStep nonUniformScaleStep(const Document &doc, std::span<const EntityId> selection,
                                  double factorX, double factorY) {
    (void)doc;
    (void)selection;
    TransformStep step;
    // Refused even when the two factors agree, because an action that sometimes
    // does the thing and sometimes refuses teaches nothing. A caller with one
    // factor has scaleStep.
    (void)factorX;
    (void)factorY;
    step.error = TransformError::NonUniform;
    return step;
}

}  // namespace paroculus
