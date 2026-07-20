// What a tag offers while it lasts.
//
// A tag is a revocable macro identity over primitives, and it owns nothing. The
// affordances it supplies — a rectangle's corner handles, its width and height
// fields — are computed from the primitives every time they are asked for, never
// stored, so there is nothing to keep in step and nothing to lose when the tag
// dissolves. Break one squaring relation and every query here stops answering;
// every segment, point and surviving constraint is exactly where it was.
//
// This lives in core for the same reason glyph placement does: render draws the
// handles, hit testing picks them, and the panel reads the same width the
// handles are showing. A rule computed in three places is a rule that comes
// apart, and the way a user finds out is by dragging a handle that is not where
// it is drawn.
#pragma once

#include <array>
#include <optional>

#include "core/document.h"
#include "core/geom.h"
#include "core/pose.h"

namespace paroculus {

// A whole rectangle tag, resolved to the primitives its affordances read.
//
// `corners` are in ring order starting from the edge the tool placed first, so
// corner i is where edge i begins and edge (i+3)%4 ends. Which is also the order
// the handles are drawn in, so a surface iterating them draws a ring rather than
// a permutation of one.
struct RectangleFrame {
    TagId tag;
    std::array<EntityId, 4> edges{};
    // The point at each corner. A corner is two coincident points, and this is
    // the one the edge leaving that corner begins at — an arbitrary but fixed
    // choice, made once here so the handle and the hit test pick the same one.
    std::array<EntityId, 4> corners{};

    // The edge whose length is the rectangle's width, and the one whose length
    // is its height. Edges 0 and 2 run across, 1 and 3 run up, which is what the
    // macro emitted; this names the representative of each pair.
    EntityId widthEdge;
    EntityId heightEdge;
};

// Resolves a rectangle tag, or nullopt when it is not a whole rectangle tag.
//
// Nullopt covers every degradation in one answer, deliberately: a surface asking
// "what does this rectangle offer" must not have to enumerate the ways it might
// have stopped being one. A broken tag offers nothing and says so once.
std::optional<RectangleFrame> rectangleFrame(const Document &doc, const TagRecord &tag);
std::optional<RectangleFrame> rectangleFrame(const Document &doc, TagId id);

// Every whole rectangle tag, in ID order. What the overlay draws handles for.
std::vector<RectangleFrame> rectangleFrames(const Document &doc);

// Whether `point` is one of the frame's corners.
//
// Either of the two coincident points that make a corner, because a corner is a
// pair and a handle that answered for one of them and not the other would work
// half the time — and which half would depend on which end the edge that
// happens to be walked first begins at.
bool isRectangleCorner(const Document &doc, const RectangleFrame &frame, EntityId point);

// Where a frame's handles are, at this pose. Nullopt when the pose cannot place
// a corner, which is a document mid-load rather than a broken tag.
std::optional<std::array<Point, 4>> rectangleHandles(const Pose &pose,
                                                     const RectangleFrame &frame);

// The rectangle's width and height at this pose, and which dimensions hold them.
//
// The dimension IDs are what "the handles drive the underlying slots" means: a
// rectangle whose width is a driving distance does not resist its own handle,
// the handle rewrites the value. A null ID means that side is undimensioned, so
// the handle moves geometry the ordinary way and the panel imposes a dimension
// when a number is typed into it.
struct RectangleSize {
    double width = 0.0;
    double height = 0.0;
    ConstraintId widthDimension;
    ConstraintId heightDimension;
};

std::optional<RectangleSize> rectangleSize(const Document &doc, const Pose &pose,
                                           const RectangleFrame &frame);

// The driving point-to-point distance over `edge`'s two endpoints, or null.
//
// The dimension a rectangle side is held by is an ordinary distance between the
// two ends of one edge — nothing about it is rectangle-specific, which is the
// point: the panel drives a relation the user could have imposed by hand and can
// delete by hand, and deleting it leaves the handle working the ordinary way.
ConstraintId edgeDimension(const Document &doc, EntityId edge);

}  // namespace paroculus
