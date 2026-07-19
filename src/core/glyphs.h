// Constraint marks, as data.
//
// No invisible constraints, ever: every constraint has to be reachable from the
// geometry it binds. A relation the user cannot see is a relation they cannot
// select, edit or argue with, and a document full of those is the "why did that
// move?" failure this project exists to avoid.
//
// The type lives in core because both sides of a seam need it and neither may
// see the other: interact decides which marks are visible, since that depends
// on selection, hover and a density budget, and render decides what a mark
// looks like. Neither layer may include the other, so the vocabulary they share
// belongs beneath both.
#pragma once

#include "core/geom.h"
#include "core/ids.h"
#include "core/taxonomy.h"

namespace paroculus {

// One mark to draw, anchored to a piece of the geometry it binds.
//
// A constraint produces one mark per operand it can sit on, not one mark
// overall: "reachable from the geometry it binds" means from each piece of it,
// and a parallel that marked only one of its two segments would be invisible
// from the other.
struct GlyphMark {
    ConstraintId constraint;
    ConstraintKind kind = ConstraintKind::Coincident;
    EntityId on;   // the operand this mark sits on
    Point anchor;  // document space; render places and sizes it in pixels

    // Emphasis, decided by the visibility policy rather than here. A mark does
    // not have an opinion about whether it matters — the overlay does.
    bool selected = false;
    bool hovered = false;
    // Declared by the placement just committed, so the user sees what happened.
    bool fresh = false;
    // A relation a placement would declare, not one it has. Preview shows
    // truth, and the truth includes which relations are about to exist.
    bool ghost = false;
};

}  // namespace paroculus
