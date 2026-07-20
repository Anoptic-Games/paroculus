// Hit testing over solved geometry.
//
// Hit-testing is not rendering: it has its own index and its own priority
// policy, because what the user can pick and what the user can see are related
// but not the same question. Adorners sit above geometry, construction is
// demoted, and a vertex beats the edge it sits on.
//
// Tolerances arrive in pixels and are inverse-transformed per query. One side
// of this file owns pixels and the other owns document units, and the boundary
// is named — every zoom bug is a leak between those two regimes.
#pragma once

#include <optional>
#include <span>
#include <vector>

#include "core/glyphs.h"
#include "core/pose.h"
#include "interact/policies.h"

namespace paroculus {

// A uniform grid over solved geometry.
//
// Rebuilt rather than maintained: a drag moves one component and the index is
// consulted between gestures, not within them, so incremental maintenance would
// buy complexity rather than time. When that stops being true the replacement
// goes behind this interface.
class SpatialIndex {
public:
    SpatialIndex() = default;

    // Indexes every entity the pose can place. Safe to call on every document
    // edit; cheap enough that stage 3 does exactly that.
    void rebuild(const Pose &pose);

    // Entities whose bounds touch the query box, in ID order. May include
    // entities that miss it — the caller does the exact test.
    std::vector<EntityId> near(Point centre, double radius) const;

    bool empty() const { return cells_.empty(); }

private:
    struct Cell {
        int64_t x = 0, y = 0;
        std::vector<EntityId> entities;
    };

    int64_t cellOf(double v) const;

    std::vector<Cell> cells_;
    double cellSize_ = 32.0;
};

struct Hit {
    EntityId entity;
    HitKind kind = HitKind::Point;
    double distance = 0.0;  // pixels

    bool valid() const { return entity.valid(); }
};

// Everything within tolerance of the cursor, best first.
//
// pose: the geometry currently on screen, in-flight values included.
// view: the transform in force, used to convert the pixel tolerances.
// screen: cursor position in pixels.
// selected: entities that currently count as selected, for the priority bump.
std::vector<Hit> hitTestAll(const Pose &pose, const SpatialIndex &index,
                            const ViewTransform &view, const Eigen::Vector2d &screen,
                            const HitPolicy &policy, const std::vector<EntityId> &selected = {});

// The single best hit, or nullopt when nothing is within tolerance.
std::optional<Hit> hitTest(const Pose &pose, const SpatialIndex &index,
                           const ViewTransform &view, const Eigen::Vector2d &screen,
                           const HitPolicy &policy,
                           const std::vector<EntityId> &selected = {});

// A constraint mark under the cursor.
struct GlyphHit {
    ConstraintId constraint;
    // The operand the picked mark sits on, so a surface can say which end of a
    // relation the user reached it from.
    EntityId on;
    double distance = 0.0;  // pixels
};

// The constraint mark nearest the cursor, or nullopt when none is within reach.
//
// Adorners sit above geometry in the hit priority, and a glyph is why: the mark
// exists precisely so the relation can be reached, and a click that fell
// through to the segment underneath would make every relation unreachable from
// the thing it binds. No invisible constraints means no unpickable ones.
//
// marks: the visible set, exactly as it was handed to render. Placement runs
//   through the same layOutGlyphs render draws with, so a mark is picked where
//   it is drawn rather than where its anchor happens to be.
// Ghost marks are never picked: they describe a relation that does not exist
//   yet, and selecting one would select nothing.
std::optional<GlyphHit> hitGlyph(std::span<const GlyphMark> marks, const ViewTransform &view,
                                 const Eigen::Vector2d &screen,
                                 const GlyphLayout &layout = {});

// Everything wholly inside a screen-space rectangle, in ID order. The marquee.
//
// Wholly inside rather than merely touched: a marquee that grabs anything it
// grazes is how a selection ends up containing things the user did not see.
std::vector<EntityId> marquee(const Pose &pose, const ViewTransform &view,
                              const Eigen::Vector2d &cornerA, const Eigen::Vector2d &cornerB);

}  // namespace paroculus
