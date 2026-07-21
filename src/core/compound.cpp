#include "core/compound.h"

#include <algorithm>
#include <cmath>

#include "core/copy.h"
#include "core/transform.h"

namespace paroculus {
namespace {

// The selected points, ordered along the run they form, deduplicated.
//
// Points only and not the closure: distributing a segment would mean
// distributing both its endpoints, which is a rhythm over two things that are
// already joined and says nothing.
//
// Ordered by where they are, not by when they were made. ID order is the
// obvious choice and it is wrong: a user who draws the left point, then the
// right one, then one in the middle has three points whose ID order is
// left-right-middle, and chaining equal gaps along that order declares that the
// right-hand point is the midpoint of the other two. The solver then yanks the
// drawing into an arrangement nobody selected, and the tag reports success. A
// distribution is a statement about the arrangement on screen, so it has to read
// the arrangement on screen.
//
// Seeds rather than a pose, for the same reason mirror reflects seeds: this is a
// document edit and a document edit is written against the committed geometry.
// Ties break by ID so the order stays total and the expansion stays
// reproducible.
std::vector<EntityId> selectedPoints(const Document &doc,
                                     std::span<const EntityId> selection) {
    std::vector<EntityId> out;
    for(EntityId id : selection) {
        const EntityRecord *r = doc.entities().find(id);
        if(r != nullptr && r->kind == EntityKind::Point) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if(out.size() < 2) return out;

    // The run's direction is the widest separation in the set, which is the only
    // axis a distribution can mean: the two points furthest apart are its ends
    // whatever order they were drawn in.
    auto seedOf = [&](EntityId id) {
        const EntityRecord *r = doc.entities().find(id);
        return Point{r->seeds[0], r->seeds[1]};
    };
    size_t bestA = 0, bestB = 1;
    double widest = -1.0;
    for(size_t i = 0; i < out.size(); i++) {
        for(size_t j = i + 1; j < out.size(); j++) {
            const Point a = seedOf(out[i]);
            const Point b = seedOf(out[j]);
            const double d = std::hypot(b.x - a.x, b.y - a.y);
            if(d > widest) {
                widest = d;
                bestA = i;
                bestB = j;
            }
        }
    }
    const Point from = seedOf(out[bestA]);
    const Point to = seedOf(out[bestB]);
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    // Every point on top of every other: no direction to order along, and the
    // ID order it falls back to is as good as any because they are all the same
    // place. distributeStep refuses it further down for being degenerate.
    if(dx == 0.0 && dy == 0.0) return out;

    std::stable_sort(out.begin(), out.end(), [&](EntityId lhs, EntityId rhs) {
        const Point a = seedOf(lhs);
        const Point b = seedOf(rhs);
        const double ta = (a.x - from.x) * dx + (a.y - from.y) * dy;
        const double tb = (b.x - from.x) * dx + (b.y - from.y) * dy;
        if(ta != tb) return ta < tb;
        return lhs < rhs;
    });
    return out;
}

EntityRecord constructionSegment(EntityId id, EntityId a, EntityId b, LayerId layer) {
    EntityRecord r;
    r.id = id;
    r.kind = EntityKind::Segment;
    r.role = Role::Construction;
    r.layer = layer;
    r.points[0] = a;
    r.points[1] = b;
    return r;
}

LayerId layerOf(const Document &doc, EntityId id) {
    const EntityRecord *r = doc.entities().find(id);
    return r != nullptr ? r->layer : LayerId();
}

}  // namespace

const char *compoundErrorName(CompoundError e) {
    switch(e) {
        case CompoundError::None: return "none";
        case CompoundError::TooFew: return "too-few";
        case CompoundError::NoAxis: return "no-axis";
        case CompoundError::WrongKinds: return "wrong-kinds";
        case CompoundError::AxisInside: return "axis-inside";
        case CompoundError::Degenerate: return "degenerate";
    }
    return "none";
}

CompoundStep distributeStep(const Document &doc, std::span<const EntityId> selection) {
    CompoundStep step;
    const std::vector<EntityId> points = selectedPoints(doc, selection);
    // Three: two points are always evenly distributed, so a distribution over
    // them declares nothing and the tag would be an affordance for a shape that
    // is not there. The same number the tag minimum names, from the same reason.
    if(points.size() < 3) {
        step.error = CompoundError::TooFew;
        return step;
    }
    // A run of zero length names no direction, so there is no arrangement to be
    // even about and the span segment would be degenerate.
    {
        const EntityRecord *first = doc.entities().find(points.front());
        const EntityRecord *last = doc.entities().find(points.back());
        if(first == nullptr || last == nullptr ||
           (first->seeds[0] == last->seeds[0] && first->seeds[1] == last->seeds[1])) {
            step.error = CompoundError::Degenerate;
            return step;
        }
    }

    uint32_t nextEntity = doc.entities().allocator().next();
    uint32_t nextConstraint = doc.constraints().allocator().next();
    const LayerId layer = layerOf(doc, points.front());

    const EntityId span(nextEntity++);
    step.commands.push_back(AddRecord<EntityRecord>{
        constructionSegment(span, points.front(), points.back(), layer)});
    step.entities.push_back(span);

    std::vector<EntityId> gaps;
    gaps.reserve(points.size() - 1);
    for(size_t i = 0; i + 1 < points.size(); i++) {
        const EntityId gap(nextEntity++);
        step.commands.push_back(AddRecord<EntityRecord>{
            constructionSegment(gap, points[i], points[i + 1], layer)});
        step.entities.push_back(gap);
        gaps.push_back(gap);
    }

    // Collinearity first, then rhythm. Order is not semantics — the solver reads
    // a set — but it is format: the expansion has to be reproducible command for
    // command or the property test comparing it against a hand-built set is
    // testing the order a loop happened to run in.
    for(size_t i = 1; i + 1 < points.size(); i++) {
        ConstraintRecord onLine;
        onLine.id = ConstraintId(nextConstraint++);
        onLine.kind = ConstraintKind::PointOnLine;
        onLine.operands[0] = points[i];
        onLine.operands[1] = span;
        step.commands.push_back(AddRecord<ConstraintRecord>{onLine});
        step.constraints.push_back(onLine.id);
    }

    for(size_t i = 0; i + 1 < gaps.size(); i++) {
        ConstraintRecord equal;
        equal.id = ConstraintId(nextConstraint++);
        equal.kind = ConstraintKind::EqualLength;
        equal.operands[0] = gaps[i];
        equal.operands[1] = gaps[i + 1];
        step.commands.push_back(AddRecord<ConstraintRecord>{equal});
        step.constraints.push_back(equal.id);
    }

    // The tag names the points as well as the construction geometry, because
    // what a distribution is about is the points. Deleting one takes its
    // relations with it, the tag's constraint list shrinks below what a rhythm
    // needs, and the tag reports itself broken — every primitive still standing.
    TagRecord tag;
    tag.id = TagId(doc.tags().allocator().next());
    tag.kind = TagKind::Distribution;
    tag.entities = points;
    tag.entities.insert(tag.entities.end(), step.entities.begin(), step.entities.end());
    tag.constraints = step.constraints;
    step.commands.push_back(AddRecord<TagRecord>{tag});
    step.tag = tag.id;
    return step;
}

EntityId mirrorAxisIn(const Document &doc, std::span<const EntityId> selection) {
    // Construction first, because an axis is construction geometry.
    //
    // PRINCIPLES settles this: a guide, a frame or an axis is ordinary geometry
    // with a render role, which is exactly why every guide capability falls out
    // of not having a guide type. So a selection holding one construction
    // segment has said which line it means, and the ordinary segments beside it
    // are the subject rather than rival candidates. Without this rule, mirroring
    // a bar about a guide is ambiguous — two segments, no way to tell them apart
    // — and the operation is unreachable for the case it exists to serve.
    EntityId construction;
    EntityId plain;
    size_t constructionCount = 0;
    size_t plainCount = 0;
    for(EntityId id : selection) {
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr || r->kind != EntityKind::Segment) continue;
        if(r->role == Role::Construction) {
            construction = id;
            constructionCount++;
        } else {
            plain = id;
            plainCount++;
        }
    }
    // Exactly one either way. Two construction segments make the axis ambiguous
    // again, and guessing which the user meant is the silent choice the surface
    // exists to ask about — nothing is returned and mirror reports no axis.
    if(constructionCount == 1) return construction;
    if(constructionCount == 0 && plainCount == 1) return plain;
    return EntityId();
}

CompoundStep mirrorStep(const Document &doc, std::span<const EntityId> selection,
                        EntityId axis) {
    CompoundStep step;
    const EntityRecord *axisRecord = doc.entities().find(axis);
    if(axisRecord == nullptr || axisRecord->kind != EntityKind::Segment) {
        step.error = CompoundError::NoAxis;
        return step;
    }
    const EntityRecord *a = doc.entities().find(axisRecord->points[0]);
    const EntityRecord *b = doc.entities().find(axisRecord->points[1]);
    if(a == nullptr || b == nullptr) {
        step.error = CompoundError::NoAxis;
        return step;
    }

    // What gets mirrored: the selection minus the axis and minus the axis's own
    // endpoints. Reflecting the axis about itself is the identity, and the
    // symmetric relation that would record it relates a point to itself.
    std::vector<EntityId> subject;
    for(EntityId id : selection) {
        if(id == axis || id == axisRecord->points[0] || id == axisRecord->points[1]) continue;
        if(doc.entities().find(id) != nullptr) subject.push_back(id);
    }
    if(subject.empty()) {
        step.error = CompoundError::TooFew;
        return step;
    }
    // The axis has to stay outside what moves, or the reflection is defined in
    // terms of geometry the same step is reflecting.
    const std::vector<EntityId> moved = transformClosure(doc, subject);
    for(EntityId id : moved) {
        if(id == axis || id == axisRecord->points[0] || id == axisRecord->points[1]) {
            step.error = CompoundError::AxisInside;
            return step;
        }
    }

    const double ox = a->seeds[0];
    const double oy = a->seeds[1];
    const double dx = b->seeds[0] - ox;
    const double dy = b->seeds[1] - oy;
    const double len2 = dx * dx + dy * dy;
    if(len2 == 0.0) {
        step.error = CompoundError::Degenerate;
        return step;
    }

    // Reflection about the line through the axis's seeds. Seeds and not the
    // pose, because this is a document edit and a document edit is written
    // against the committed geometry — placing images against an in-flight drag
    // would bake a transient into the record.
    const auto reflect = [ox, oy, dx, dy, len2](Point p) {
        const double px = p.x - ox;
        const double py = p.y - oy;
        const double t = (px * dx + py * dy) / len2;
        const double cx = t * dx;
        const double cy = t * dy;
        return Point{ox + 2.0 * cx - px, oy + 2.0 * cy - py};
    };

    const CopyStep copy = copyStep(doc, subject, CopyPlacement{reflect, true});
    if(copy.empty()) {
        step.error = CompoundError::TooFew;
        return step;
    }
    step.commands = copy.commands;
    // The copy's drops are the mirror's drops: a reflection sheds the null-reference
    // axis relations it cannot preserve, and that count is the user's to know.
    step.droppedConstraints = copy.droppedConstraints;
    step.droppedRegions = copy.droppedRegions;
    step.droppedTags = copy.droppedTags;

    uint32_t nextConstraint = doc.constraints().allocator().next() +
                              static_cast<uint32_t>(copy.constraints.size());

    // Originals in ID order, so the symmetric relations come out in a fixed
    // sequence whatever order the map happened to be walked in.
    std::vector<EntityId> originals;
    originals.reserve(copy.entities.size());
    for(const auto &[from, to] : copy.entities) originals.push_back(from);
    std::sort(originals.begin(), originals.end());

    TagRecord tag;
    tag.id = TagId(doc.tags().allocator().next() + static_cast<uint32_t>(copy.tags.size()));
    tag.kind = TagKind::Mirror;

    for(EntityId original : originals) {
        const EntityRecord *r = doc.entities().find(original);
        if(r == nullptr || r->kind != EntityKind::Point) continue;
        const EntityId image = copy.entities.at(original);

        ConstraintRecord sym;
        sym.id = ConstraintId(nextConstraint++);
        sym.kind = ConstraintKind::SymmetricAboutLine;
        sym.operands[0] = original;
        sym.operands[1] = image;
        sym.operands[2] = axis;
        step.commands.push_back(AddRecord<ConstraintRecord>{sym});
        step.constraints.push_back(sym.id);

        tag.entities.push_back(original);
        tag.entities.push_back(image);
        tag.constraints.push_back(sym.id);
    }

    if(tag.constraints.empty()) {
        // Nothing in the selection carried a position of its own, so there is
        // nothing a reflection could hold. Refused whole rather than leaving an
        // unconstrained copy behind, which is a duplicate the user did not ask
        // for.
        step.commands.clear();
        step.error = CompoundError::WrongKinds;
        return step;
    }

    step.entities = copy.copiedEntities();
    step.commands.push_back(AddRecord<TagRecord>{tag});
    step.tag = tag.id;
    return step;
}

}  // namespace paroculus
