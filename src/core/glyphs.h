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

#include <span>
#include <string_view>
#include <vector>

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

    // Whether this unvalued mark should carry its mnemonic label beside the
    // shape. Set by the overlay's budget, not here: a label is dropped before
    // the mark it labels as density tightens, so which marks get one is a
    // property of the whole overlay rather than of the mark. Ignored on valued
    // marks, which always carry their number.
    bool showLabel = false;

    // The per-anchor overflow mark: the fan around a crowded anchor caps at a
    // small count and the excess collapses into one ⋯ mark drawn at the next fan
    // slot. It stands for no single constraint — `constraint` is null — so it is
    // never a relation selection; picking it opens the inspector on `on`, the
    // anchor's operand, so detailed inspection is one click from the crowd. Fan
    // placement stays layOutGlyphs's, so render and hit testing agree on where
    // the ⋯ sits exactly as they agree on the marks.
    bool overflow = false;
};

// The short mnemonic a mark's kind reads as, for the unvalued marks a loose
// budget labels. Valued kinds return empty — their label is their value, drawn
// by render from the pose. Stable enough to draw beside the shape without
// teaching a second vocabulary: it is the kind's convention spelled in a glyph
// or two, not its full title.
std::string_view glyphMnemonic(ConstraintKind kind);

// How marks are placed around the geometry they annotate.
//
// Pixel quantities, like every other adorner measurement: a glyph is the same
// size and the same distance from its anchor at every magnification, which is
// what makes it as pickable when zoomed out as when zoomed in.
struct GlyphLayout {
    // How far a mark sits from its anchor, so it never hides the vertex or edge
    // it is describing.
    double offset = 9.0;
    // How far apart successive marks on one anchor fan, in radians.
    double fanStep = 1.3;
    double fanStart = -1.0;
    // How close a click has to come to pick one.
    double radius = 7.0;
};

// Where each mark actually sits on screen, in pixels, in the order given.
//
// Marks sharing an anchor fan out around it: a vertex with three relations has
// to read as three, and stacking them would make two of the three invisible and
// all three unpickable.
//
// This lives in core for the same reason the pose does. Render draws marks at
// these positions and hit testing has to pick them at these positions, and if
// the two computed the fan-out separately the user would click a mark and
// select nothing — the same "picks one thing, selects another" failure the pose
// exists to rule out. Neither layer may include the other, so the arithmetic
// they share belongs beneath both.
std::vector<Eigen::Vector2d> layOutGlyphs(std::span<const GlyphMark> marks,
                                          const ViewTransform &view,
                                          const GlyphLayout &layout = {});

}  // namespace paroculus
