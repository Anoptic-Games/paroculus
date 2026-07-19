// Which constraint marks to draw this frame.
//
// The visible set is computed as a set, per frame, under a budget — a policy
// over the whole overlay rather than a boolean on each mark. That is the only
// arrangement that degrades sensibly: per-glyph rules produce a screen that is
// either empty at scale or unreadable at scale, depending on which way each
// rule was tuned, and no amount of tuning fixes it because the rules cannot see
// each other.
//
// Placement is in document units here. Where exactly a mark sits in pixels and
// what it looks like are render's business; which marks exist and which survive
// are not.
#pragma once

#include <span>
#include <vector>

#include "core/document.h"
#include "core/glyphs.h"
#include "core/pose.h"
#include "interact/policies.h"
#include "interact/snap.h"

namespace paroculus {

// What the overlay knows about the user's attention this frame.
struct GlyphContext {
    std::span<const EntityId> selected;
    EntityId hovered;
    // Constraints the last placement declared. Fresh marks outrank ordinary
    // ones, because the moment a relation appears is the moment it most needs
    // to be seen.
    std::span<const ConstraintId> fresh;
    Point cursor;
    bool haveCursor = false;
};

// The marks a constraint contributes, one per operand it can sit on.
// Appends rather than returns, so the caller controls the allocation.
void marksFor(const Document &doc, const Pose &pose, const ConstraintRecord &constraint,
              std::vector<GlyphMark> &out);

// The visible set, ranked and truncated to the budget.
//
// width, height: viewport pixels, which is what the density budget is against.
std::vector<GlyphMark> visibleGlyphs(const Document &doc, const Pose &pose,
                                     const ViewTransform &view, double width, double height,
                                     const GlyphContext &context, const GlyphPolicy &policy);

// Marks for relations a placement would declare but has not yet.
//
// candidates: what inference is proposing. placement: where the point would go.
// segmentFrom / haveSegment: the in-flight segment, for the direction-valued
// kinds, which sit on the segment rather than on either end of it.
std::vector<GlyphMark> ghostGlyphs(std::span<const SnapCandidate> candidates, Point placement,
                                   bool haveSegment, Point segmentFrom);

}  // namespace paroculus
