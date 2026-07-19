#include "interact/glyphs.h"

#include <algorithm>
#include <cmath>

namespace paroculus {
namespace {

// Where a mark sits on one operand: on a point, that point; on anything with
// two ends, the middle of it. The middle rather than an end because a mark at
// an endpoint collides with every other relation binding the same vertex, and
// vertices are where relations cluster.
std::optional<Point> anchorOn(const Document &doc, const Pose &pose, EntityId id) {
    if(const std::optional<Point> p = pose.point(id)) return p;
    if(const auto ends = pose.segment(id)) {
        return Point{(ends->first.x + ends->second.x) * 0.5,
                     (ends->first.y + ends->second.y) * 0.5};
    }
    const EntityRecord *e = doc.entities().find(id);
    if(e != nullptr && pose.radius(id)) return pose.point(e->points[0]);
    return std::nullopt;
}

bool contains(std::span<const EntityId> haystack, EntityId needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

bool contains(std::span<const ConstraintId> haystack, ConstraintId needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

}  // namespace

void marksFor(const Document &doc, const Pose &pose, const ConstraintRecord &constraint,
              std::vector<GlyphMark> &out) {
    const ConstraintKindInfo &info = constraintInfo(constraint.kind);
    for(size_t i = 0; i < boundOperandCount(constraint); i++) {
        const EntityId operand = constraint.operands[i];
        if(!operand.valid()) continue;
        const std::optional<Point> anchor = anchorOn(doc, pose, operand);
        if(!anchor) continue;

        GlyphMark mark;
        mark.constraint = constraint.id;
        mark.kind = constraint.kind;
        mark.on = operand;
        mark.anchor = *anchor;
        out.push_back(mark);
    }
}

std::vector<GlyphMark> visibleGlyphs(const Document &doc, const Pose &pose,
                                     const ViewTransform &view, double width, double height,
                                     const GlyphContext &context, const GlyphPolicy &policy) {
    std::vector<GlyphMark> marks;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        marksFor(doc, pose, c, marks);
    }

    // Off-screen marks are not competing for the budget: a relation the user
    // cannot see is not clutter, and letting it displace one they can would
    // make scrolling change which relations are legible.
    marks.erase(std::remove_if(marks.begin(), marks.end(),
                               [&](const GlyphMark &m) {
                                   const Eigen::Vector2d s = view.toScreen(m.anchor);
                                   return s.x() < 0.0 || s.y() < 0.0 || s.x() > width ||
                                          s.y() > height;
                               }),
                marks.end());

    std::vector<std::pair<double, size_t>> ranked;
    ranked.reserve(marks.size());
    for(size_t i = 0; i < marks.size(); i++) {
        GlyphMark &m = marks[i];
        m.selected = contains(context.selected, m.on);
        m.hovered = context.hovered.valid() && context.hovered == m.on;
        m.fresh = contains(context.fresh, m.constraint);

        double score = 0.0;
        if(m.selected) score += policy.selectedWeight;
        if(m.hovered) score += policy.hoveredWeight;
        if(m.fresh) score += policy.freshWeight;
        if(context.haveCursor) {
            const double d =
                (view.toScreen(m.anchor) - view.toScreen(context.cursor)).norm();
            // Nearer is better, and the falloff is gentle so proximity settles
            // ties rather than dominating the emphasis weights.
            score += policy.proximityWeight * 1000.0 / (1.0 + d);
        }
        ranked.emplace_back(score, i);
    }

    // Ties break on constraint id and then operand id, never on iteration
    // order, so the same frame always draws the same set.
    std::stable_sort(ranked.begin(), ranked.end(),
                     [&](const std::pair<double, size_t> &a, const std::pair<double, size_t> &b) {
                         if(a.first != b.first) return a.first > b.first;
                         const GlyphMark &ma = marks[a.second];
                         const GlyphMark &mb = marks[b.second];
                         if(ma.constraint != mb.constraint) return ma.constraint < mb.constraint;
                         return ma.on < mb.on;
                     });

    const double megapixels = std::max(width * height, 0.0) / 1.0e6;
    size_t budget = static_cast<size_t>(policy.density * megapixels);
    budget = std::min(budget, policy.hardCap);
    if(ranked.size() > budget) ranked.resize(budget);

    std::vector<GlyphMark> out;
    out.reserve(ranked.size());
    for(const auto &[score, index] : ranked) out.push_back(marks[index]);
    return out;
}

std::vector<GlyphMark> ghostGlyphs(std::span<const SnapCandidate> candidates, Point placement,
                                   PlacementRoles roles, Point segmentFrom) {
    std::vector<GlyphMark> out;
    for(const SnapCandidate &c : candidates) {
        // Only what would actually be declared. An offer the user has not
        // confirmed is a suggestion in the strip, not a relation about to
        // exist, and ghosting it would promise something commit will not do.
        if(!c.autoCommits()) continue;

        // The commit's own resolver, asked a click early. It answers both
        // questions at once — whether this candidate survives the placement's
        // shape, and which relation it turns out to be — so the ghost cannot
        // name a kind the commit would not write. Placement-only kinds and
        // candidates whose subject the placement never creates both fall out
        // here as nullopt rather than needing a rule of their own.
        const std::optional<ConstraintKind> kind = declaredKind(c, roles);
        if(!kind) continue;

        GlyphMark mark;
        mark.kind = *kind;
        mark.ghost = true;
        if(c.subject == SnapSubject::PlacedSegment) {
            mark.anchor = Point{(segmentFrom.x + placement.x) * 0.5,
                                (segmentFrom.y + placement.y) * 0.5};
        } else {
            mark.anchor = placement;
        }
        out.push_back(mark);
    }
    return out;
}

}  // namespace paroculus
