// The bake: the one destructive path in the whole tool, and it leads out.
//
// Everywhere else, an equivalence is honoured by never creating the second
// representation. Export is the exception that has to exist, because SVG and its
// friends have no constraints, no slots and no live region algebra — so
// something has to flatten. What matters is where that lives: outside the
// document, producing a value nobody can edit, counting what it destroyed.
//
// Baking never mutates a document and there is no in-document bake. A boolean
// that consumed its operands to mint a new outline would be exactly the lossy
// converter this project refuses to build, and having one available "just for
// performance" is how it becomes the representation.
//
// Stage 6 lands the projection and the loss report; stage 8 lands the writers
// that turn one into a file. The polygon boolean a union or an intersect
// finally needs is deliberately not here: it belongs with the exporter, which
// has a path library, and core has no business growing one.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/document.h"
#include "core/pose.h"

namespace paroculus {

// One filled ring, in document coordinates, closed implicitly.
//
// `combine` is how this ring meets the ones before it within the same bake
// group: Outline for a fill in its own right, and the composite's operation for
// each operand of a composite. Carried rather than resolved because resolving it
// is the polygon boolean that belongs to the exporter.
struct BakedFill {
    std::vector<Point> ring;
    CompositeOp combine = CompositeOp::Outline;
    size_t group = 0;  // operands of one composite share a group
    LayerId layer;
    uint32_t colour = 0u;
    bool punch = false;
};

struct BakedStroke {
    Point from;
    Point to;
    LayerId layer;
    uint32_t colour = 0u;
    double width = 1.0;
};

// What the bake produced and what it cost.
//
// The counts are the honest half. A user who bakes has traded a program for a
// picture, and the trade is worth reporting in the same breath as the result —
// the same policy that has deletion report what it took with it.
struct Bake {
    std::vector<BakedFill> fills;
    std::vector<BakedStroke> strokes;

    size_t constraintsDropped = 0;
    size_t parametersDropped = 0;
    size_t regionsFlattened = 0;
    size_t tagsDropped = 0;

    // Regions that could not be flattened because they no longer enclose
    // anything. Broken geometry does not silently become an empty area.
    size_t regionsBroken = 0;
};

// Flattens the visible document at its current pose.
//
// Hidden layers are left out — a bake is what the drawing looks like — while
// locked ones are ordinary geometry, since locking says nothing about
// visibility. Construction geometry is excluded, as it is from every export, by
// role.
Bake bakeForExport(const Document &doc, const Pose &pose);

}  // namespace paroculus
