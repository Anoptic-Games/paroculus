#include "core/tags.h"

#include <cmath>

#include "core/composition.h"
#include "core/topology.h"

namespace paroculus {
namespace {

// Whether a constraint of this kind names `edge` as its required segment.
bool holds(const ConstraintRecord &c, ConstraintKind kind, EntityId edge) {
    return c.kind == kind && c.operands[0] == edge;
}

}  // namespace

std::optional<RectangleFrame> rectangleFrame(const Document &doc, const TagRecord &tag) {
    if(tag.kind != TagKind::Rectangle) return std::nullopt;
    // One question, asked in one place. A frame is offered exactly when the tag
    // is whole, so a surface can never be drawing handles for a rectangle the
    // degradation query calls broken.
    if(tagState(doc, tag) != TagState::Whole) return std::nullopt;

    RectangleFrame frame;
    frame.tag = tag.id;
    size_t found = 0;
    for(EntityId id : tag.entities) {
        const EntityRecord *r = doc.entities().find(id);
        if(r == nullptr || r->kind != EntityKind::Segment) continue;
        if(found == 4) return std::nullopt;  // more edges than a rectangle has
        frame.edges[found] = id;
        frame.corners[found] = r->points[0];
        found++;
    }
    if(found != 4) return std::nullopt;

    // Which pair runs across and which runs up is read from the relations that
    // square it, not from the order they were emitted in. A rectangle retargeted
    // to a cluster frame still declares horizontal and vertical — against a
    // different axis — so this keeps answering after a rotation, which is
    // exactly the case a positional assumption would get wrong.
    for(ConstraintId id : tag.constraints) {
        const ConstraintRecord *c = doc.constraints().find(id);
        if(c == nullptr) continue;
        for(EntityId edge : frame.edges) {
            if(!frame.widthEdge.valid() && holds(*c, ConstraintKind::Horizontal, edge)) {
                frame.widthEdge = edge;
            }
            if(!frame.heightEdge.valid() && holds(*c, ConstraintKind::Vertical, edge)) {
                frame.heightEdge = edge;
            }
        }
    }
    if(!frame.widthEdge.valid() || !frame.heightEdge.valid()) return std::nullopt;
    return frame;
}

std::optional<RectangleFrame> rectangleFrame(const Document &doc, TagId id) {
    const TagRecord *t = doc.tags().find(id);
    if(t == nullptr) return std::nullopt;
    return rectangleFrame(doc, *t);
}

std::vector<RectangleFrame> rectangleFrames(const Document &doc) {
    std::vector<RectangleFrame> out;
    for(const TagRecord &t : doc.tags().records()) {
        if(std::optional<RectangleFrame> f = rectangleFrame(doc, t)) out.push_back(*f);
    }
    return out;
}

bool isRectangleCorner(const Document &doc, const RectangleFrame &frame, EntityId point) {
    if(!point.valid()) return false;
    for(EntityId edge : frame.edges) {
        const EntityRecord *e = doc.entities().find(edge);
        if(e == nullptr) continue;
        if(e->points[0] == point || e->points[1] == point) return true;
    }
    return false;
}

std::optional<std::array<Point, 4>> rectangleHandles(const Pose &pose,
                                                     const RectangleFrame &frame) {
    std::array<Point, 4> out{};
    for(size_t i = 0; i < 4; i++) {
        const std::optional<Point> p = pose.point(frame.corners[i]);
        if(!p) return std::nullopt;
        out[i] = *p;
    }
    return out;
}

ConstraintId edgeDimension(const Document &doc, EntityId edge) {
    const EntityRecord *e = doc.entities().find(edge);
    if(e == nullptr || e->kind != EntityKind::Segment) return ConstraintId();

    // Matched by coincidence, never by point identity — the same rule a region's
    // ring walk follows, and here for the same reason.
    //
    // A rectangle's corner is two separate points joined by a coincidence, which
    // is what lets a corner be opened by deleting one relation. So a width the
    // user dimensioned by clicking two corners may name either point of either
    // pair, and identity matching finds it only if the hit test happened to hand
    // over the two this edge owns. The rest of the time the panel reports the
    // side undimensioned, the handle saturates against a relation it does not
    // know about, and typing a width adds a *second* distance over the same
    // length — over-constraining the rectangle with two records meaning one
    // thing.
    Topology topology(doc);
    const EntityId a = e->points[0];
    const EntityId b = e->points[1];
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind != ConstraintKind::PointPointDistance || !c.driving) continue;
        // Either order: a distance between A and B is the distance between B and
        // A, which is exactly why the taxonomy does not call the kind
        // order-sensitive.
        const bool forward = topology.coincident(c.operands[0], a) &&
                             topology.coincident(c.operands[1], b);
        const bool backward = topology.coincident(c.operands[0], b) &&
                              topology.coincident(c.operands[1], a);
        if(forward || backward) return c.id;
    }
    return ConstraintId();
}

std::optional<RectangleSize> rectangleSize(const Document &doc, const Pose &pose,
                                           const RectangleFrame &frame) {
    const std::optional<std::pair<Point, Point>> w = pose.segment(frame.widthEdge);
    const std::optional<std::pair<Point, Point>> h = pose.segment(frame.heightEdge);
    if(!w || !h) return std::nullopt;

    RectangleSize size;
    size.width = std::hypot(w->second.x - w->first.x, w->second.y - w->first.y);
    size.height = std::hypot(h->second.x - h->first.x, h->second.y - h->first.y);
    size.widthDimension = edgeDimension(doc, frame.widthEdge);
    size.heightDimension = edgeDimension(doc, frame.heightEdge);
    return size;
}

}  // namespace paroculus
