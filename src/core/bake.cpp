#include "core/bake.h"

#include "core/composition.h"

namespace paroculus {

namespace {

// A region's ring in document coordinates, or empty if it does not enclose.
std::vector<Point> ringOf(const Document &doc, const Pose &pose, const RegionRecord &region) {
    std::vector<Point> out;
    const std::optional<std::vector<EntityId>> ring = boundaryRing(doc, region);
    if(!ring) return out;
    out.reserve(ring->size());
    for(EntityId p : *ring) {
        const std::optional<Point> at = pose.point(p);
        if(!at) return {};
        out.push_back(*at);
    }
    return out;
}

const StyleRecord *styleOf(const Document &doc, StyleId id) {
    return id.valid() ? doc.styles().find(id) : nullptr;
}

}  // namespace

Bake bakeForExport(const Document &doc, const Pose &pose) {
    Bake out;

    // Fills first, layer by layer and by z within each, so a consumer that
    // simply paints the list in order gets the composition it saw on screen.
    // The order is the whole of what survives of the layer model: after the
    // bake there are no layers, only a sequence.
    size_t group = 0;
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

            // A composite contributes its operands, each tagged with how it
            // meets the others. One group per composite, so the exporter knows
            // which rings belong to which boolean.
            std::vector<const RegionRecord *> pending{&region};
            const size_t mine = group++;
            while(!pending.empty()) {
                const RegionRecord *r = pending.back();
                pending.pop_back();
                if(r->op == CompositeOp::Outline) {
                    BakedFill fill;
                    fill.ring = ringOf(doc, pose, *r);
                    if(fill.ring.empty()) continue;
                    fill.combine = r == &region ? CompositeOp::Outline : region.op;
                    fill.group = mine;
                    fill.layer = layer;
                    fill.colour = colour;
                    fill.punch = region.punch;
                    out.fills.push_back(std::move(fill));
                    continue;
                }
                for(RegionId o : r->operands) {
                    if(const RegionRecord *child = doc.regions().find(o)) pending.push_back(child);
                }
            }
            out.regionsFlattened++;
        }
    }

    for(const EntityRecord &e : doc.entities().records()) {
        if(e.role == Role::Construction) continue;
        if(!layerVisible(doc, e.layer)) continue;
        const std::optional<std::pair<Point, Point>> segment = pose.segment(e.id);
        if(!segment) continue;
        const StyleRecord *style = styleOf(doc, e.style);
        BakedStroke stroke;
        stroke.from = segment->first;
        stroke.to = segment->second;
        stroke.layer = e.layer;
        stroke.colour = style != nullptr ? style->strokeColor : 0xff000000u;
        stroke.width = style != nullptr ? doc.evaluate(style->strokeWidth).value_or(1.0) : 1.0;
        out.strokes.push_back(stroke);
    }

    // What the picture will not know. Counted rather than described, on the
    // same rule that has a deletion report a number instead of opening a dialog.
    out.constraintsDropped = doc.constraints().size();
    out.parametersDropped = doc.parameters().size();
    out.tagsDropped = doc.tags().size();
    return out;
}

}  // namespace paroculus
