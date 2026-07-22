#include "interact/glyphs.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "core/composition.h"
#include "core/taxonomy.h"

namespace paroculus {
namespace {

// Where a mark sits on one operand: on a point, that point; on anything with
// two ends, the middle of it. The middle rather than an end because a mark at
// an endpoint collides with every other relation binding the same vertex, and
// vertices are where relations cluster.
//
// An arc has two ends, so its middle is the midpoint of its sweep, on the rim
// where the arc is actually drawn. A circle has none, so its centre is the only
// distinguished point it offers. Both must resolve to something: an operand
// with no anchor is a constraint invisible from the geometry it binds, which is
// the one thing mark-per-operand exists to rule out.
std::optional<Point> anchorOn(const Document &doc, const Pose &pose, EntityId id) {
    if(const std::optional<Point> p = pose.point(id)) return p;
    if(const auto ends = pose.segment(id)) {
        return Point{(ends->first.x + ends->second.x) * 0.5,
                     (ends->first.y + ends->second.y) * 0.5};
    }
    if(const auto g = pose.arc(id)) {
        const double middle = g->startAngle + g->sweep * 0.5;
        return Point{g->centre.x + g->radius * std::cos(middle),
                     g->centre.y + g->radius * std::sin(middle)};
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
        // A mark goes where its operand is, so a mark on hidden geometry has
        // nowhere honest to be. It floated over empty space and stayed
        // clickable, selecting a relation on geometry the user could neither see
        // nor pick — and it spent a slot in the glyph budget doing it.
        //
        // Per operand rather than per constraint, because the constraint is not
        // hidden: a relation binding a hidden operand and a visible one still
        // marks the visible one, which is what keeps "no invisible constraints"
        // true of everything the user can actually act on. That a hidden operand
        // is still constraining is said by the influence indication instead,
        // which is the surface built for exactly this.
        if(!isVisible(doc, operand)) continue;
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

VisibleGlyphs visibleGlyphs(const Document &doc, const Pose &pose, const ViewTransform &view,
                            double width, double height, const GlyphContext &context,
                            const GlyphPolicy &policy) {
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

    // Distinct relations with a mark on screen: the M of "N of M". Counted over
    // constraints rather than marks, because a two-operand relation drawing two
    // marks is one relation, and the readout is about relations.
    std::set<ConstraintId> onScreen;
    for(const GlyphMark &m : marks) onScreen.insert(m.constraint);

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

    // The per-anchor fan cap, applied to the ranked survivors: the highest-ranked
    // marks at each anchor fan, and the excess collapses into one ⋯. Grouped by
    // screen position the way layOutGlyphs fans — half a pixel is one joint — so
    // the cap and the placement agree about which marks share an anchor. The ⋯ is
    // synthesized here rather than in the layout because whether a fan overflows
    // is a budget question, while where its ⋯ sits is a placement one.
    struct Anchor {
        Eigen::Vector2d screen;
        Point at;
        EntityId on;
        size_t kept = 0;
        bool overflowed = false;
    };
    std::vector<Anchor> anchors;

    VisibleGlyphs result;
    result.total = onScreen.size();
    result.marks.reserve(ranked.size());
    std::set<ConstraintId> shownConstraints;

    for(const auto &[score, index] : ranked) {
        const GlyphMark &m = marks[index];
        const Eigen::Vector2d screen = view.toScreen(m.anchor);
        Anchor *bucket = nullptr;
        for(Anchor &a : anchors) {
            if((a.screen - screen).norm() < 0.5) {
                bucket = &a;
                break;
            }
        }
        if(bucket == nullptr) {
            anchors.push_back(Anchor{screen, m.anchor, m.on, 0, false});
            bucket = &anchors.back();
        }
        if(bucket->kept >= policy.fanLimit) {
            // The first drop at this anchor is what earns it a ⋯; the rest are
            // silently folded into that one mark.
            bucket->overflowed = true;
            continue;
        }
        bucket->kept++;
        result.marks.push_back(m);
        shownConstraints.insert(m.constraint);
    }

    // One ⋯ per overflowing anchor, appended after the kept marks so it lands at
    // fan index fanLimit — layOutGlyphs counts the fanLimit marks that precede it
    // on the same anchor and fans it into the next slot, wherever in the list it
    // sits. It carries the anchor's operand so a pick opens the crowd there.
    for(const Anchor &a : anchors) {
        if(!a.overflowed) continue;
        GlyphMark mark;
        mark.on = a.on;
        mark.anchor = a.at;
        mark.overflow = true;
        result.marks.push_back(mark);
    }

    result.shown = shownConstraints.size();

    // Mnemonic labels ride the same budget: they appear only while the overlay is
    // loose and are the first thing to drop as it tightens. A single density
    // gate over the whole overlay rather than a per-mark rule, because per-glyph
    // rules cannot see each other. Valued marks are untouched — their label is
    // their number, which is not a budget's to withhold.
    const double shownDensity = megapixels > 0.0 ? result.shown / megapixels : 0.0;
    if(shownDensity < policy.labelDensity) {
        for(GlyphMark &m : result.marks) {
            if(m.overflow) continue;
            if(constraintInfo(m.kind).valueArity == 1) continue;
            if(glyphMnemonic(m.kind).empty()) continue;
            m.showLabel = true;
        }
    }

    return result;
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
