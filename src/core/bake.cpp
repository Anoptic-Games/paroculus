#include "core/bake.h"

#include "core/composition.h"

#include <cmath>

namespace paroculus {

namespace {

const StyleRecord *styleOf(const Document &doc, StyleId id) {
    return id.valid() ? doc.styles().find(id) : nullptr;
}

// How finely a curve is broken into straight runs, as steps per full turn.
//
// Fixed rather than derived from the view the way render's is: a bake has no
// zoom to be right at, and a tessellation that depended on the window would make
// the same document export differently from two sessions. Fine enough that the
// chord error is well under a thousandth of the radius, which is past what any
// vector format's consumers will notice.
constexpr int STEPS_PER_TURN = 128;
constexpr double TWO_PI = 2.0 * M_PI;

int stepsFor(double sweep) {
    const double turns = std::fabs(sweep) / TWO_PI;
    return std::max(3, static_cast<int>(std::ceil(turns * STEPS_PER_TURN)));
}

// A region's ring in document coordinates, or empty if it does not enclose.
//
// Curved edges are flattened along their sweep rather than across their chord.
// A bake is the one lossy path in the tool and it is honest about being one, but
// the loss it is allowed is precision, not area: exporting an arc boundary as
// its chord would ship a different shape from the one on screen and count it as
// a success.
std::vector<Point> ringOf(const Document &doc, const Pose &pose, const RegionRecord &region) {
    std::vector<Point> out;
    const std::optional<std::vector<BoundaryStep>> ring = boundaryRing(doc, region);
    if(!ring) return out;
    out.reserve(ring->size());
    for(const BoundaryStep &step : *ring) {
        if(const std::optional<CurveRun> run = curveRunOf(pose, step)) {
            const int steps = stepsFor(run->sweep);
            // The last sample is the next edge's first joint, so it is left to
            // that edge to contribute — or, for a closed curve, to the ring
            // closing on itself.
            for(int i = 0; i < steps; i++) {
                const double angle = run->startAngle + run->sweep * i / steps;
                out.push_back(Point{run->centre.x + run->radius * std::cos(angle),
                                    run->centre.y + run->radius * std::sin(angle)});
            }
            continue;
        }
        const std::optional<Point> at = pose.point(step.from);
        if(!at) return {};
        out.push_back(*at);
    }
    return out;
}


}  // namespace

Bake bakeForExport(const Document &doc, const Pose &pose) {
    Bake out;

    // One counter for fills and groups both, so operand order survives the split
    // into two lists. Stamped in creation order, which the emit recursion walks
    // in operand order.
    size_t nextSeq = 0;

    // Fills first, layer by layer and by z within each, so a consumer that
    // simply paints the list in order gets the composition it saw on screen.
    // The order is the whole of what survives of the layer model: after the
    // bake there are no layers, only a sequence.
    for(LayerId layer : layerOrder(doc)) {
        if(!layerVisible(doc, layer)) continue;
        for(RegionId id : regionOrder(doc, layer)) {
            const RegionRecord &region = *doc.regions().find(id);
            if(regionState(doc, region) != RegionState::Whole) {
                out.regionsBroken++;
                continue;
            }
            const StyleRecord *style = styleOf(doc, region.style);
            const uint32_t colour = style != nullptr ? style->fillColor : 0u;

            // A composite contributes its operands, each tagged with the group
            // it is an operand of, and a nested composite opens a group of its
            // own naming the one above it. A stack that popped operands in
            // reverse and tagged every descendant with the *top* composite's
            // operation baked Intersect(A, Union(C,D)) as A∩C∩D and put a
            // subtract's subtrahends before its minuend — three faults the
            // exporter would consume as truth.
            const size_t root = out.groups.size();
            out.groups.push_back(BakedGroup{region.op, NO_BAKE_GROUP, nextSeq++});

            auto emit = [&](const RegionRecord &r, size_t into, auto &&self) -> void {
                if(r.op == CompositeOp::Outline) {
                    BakedFill fill;
                    fill.ring = ringOf(doc, pose, r);
                    if(fill.ring.empty()) return;
                    fill.combine = out.groups[into].op;
                    fill.group = into;
                    fill.seq = nextSeq++;
                    fill.layer = layer;
                    fill.colour = colour;
                    fill.punch = region.punch;
                    out.fills.push_back(std::move(fill));
                    return;
                }
                const size_t mine = out.groups.size();
                out.groups.push_back(BakedGroup{r.op, into, nextSeq++});
                // Operand order, which is what makes the first ring of a
                // subtract group the thing being cut. Cycles are refused by
                // validation, so the recursion terminates.
                for(RegionId o : r.operands) {
                    if(const RegionRecord *child = doc.regions().find(o)) self(*child, mine, self);
                }
            };

            if(region.op == CompositeOp::Outline) {
                emit(region, root, emit);
            } else {
                for(RegionId o : region.operands) {
                    if(const RegionRecord *child = doc.regions().find(o)) emit(*child, root, emit);
                }
            }
            out.regionsFlattened++;
        }
    }

    // Everything stroked on screen, which is segments and curves alike. Emitting
    // segments only meant circles and arcs were absent from the export with
    // nothing counting them — a silent loss, in the one projection whose whole
    // contract is to say what it destroyed.
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.role == Role::Construction) continue;
        if(!layerVisible(doc, e.layer)) continue;
        const StyleRecord *style = styleOf(doc, e.style);

        BakedStroke stroke;
        stroke.layer = e.layer;
        stroke.colour = style != nullptr ? style->strokeColor : 0xff000000u;
        stroke.width = style != nullptr ? doc.evaluate(style->strokeWidth).value_or(1.0) : 1.0;

        auto run = [&](Point from, Point to) {
            stroke.from = from;
            stroke.to = to;
            out.strokes.push_back(stroke);
        };
        // A curve as a closed or open polyline. The step is angular, so a small
        // arc costs a few runs and a full circle costs a turn's worth.
        auto sweepInto = [&](Point centre, double radius, double from, double sweep) {
            const int steps = stepsFor(sweep);
            Point previous{centre.x + radius * std::cos(from),
                           centre.y + radius * std::sin(from)};
            for(int i = 1; i <= steps; i++) {
                const double angle = from + sweep * i / steps;
                const Point next{centre.x + radius * std::cos(angle),
                                 centre.y + radius * std::sin(angle)};
                run(previous, next);
                previous = next;
            }
        };

        if(const std::optional<std::pair<Point, Point>> segment = pose.segment(e.id)) {
            run(segment->first, segment->second);
            continue;
        }
        if(const std::optional<Pose::ArcGeometry> arc = pose.arc(e.id)) {
            sweepInto(arc->centre, arc->radius, arc->startAngle, arc->sweep);
            continue;
        }
        if(const std::optional<double> radius = pose.radius(e.id)) {
            const std::optional<Point> centre = pose.point(e.points[0]);
            if(!centre) continue;
            sweepInto(*centre, *radius, 0.0, TWO_PI);
        }
    }

    // What the picture will not know. Counted rather than described, on the
    // same rule that has a deletion report a number instead of opening a dialog.
    out.constraintsDropped = doc.constraints().size();
    out.parametersDropped = doc.parameters().size();
    out.tagsDropped = doc.tags().size();
    return out;
}

}  // namespace paroculus
