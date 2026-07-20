// Composition in the model: layers, z-order, the region algebra, degradation.
//
// Layers and groups are organization, not semantics. What that means in
// practice is asserted here from the model side — a layer changes what is drawn
// and in what order and nothing else, membership survives what it names, and
// closure is decided by coincidence rather than by coordinates.
//
// The algebra is the other half. Union, intersect and subtract are records over
// live operands, so the properties worth pinning are that nothing is consumed,
// that a composite is exactly as whole as what it combines, and that the whole
// arrangement round-trips through the file.
#include <doctest/doctest.h>

#include <cmath>

#include "core/bake.h"
#include "core/composition.h"
#include "core/persist.h"
#include "core/topology.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

LayerId addLayer(Document &doc, const char *name, int32_t order) {
    LayerRecord l;
    l.name = name;
    l.order = order;
    const CommandResult r = doc.apply(AddRecord<LayerRecord>{l});
    REQUIRE(r.ok());
    return LayerId(r.allocated);
}

RegionId addOutline(Document &doc, std::vector<EntityId> boundary, LayerId layer = LayerId(),
                    int32_t z = 0) {
    RegionRecord r;
    r.boundary = std::move(boundary);
    r.layer = layer;
    r.z = z;
    const CommandResult result = doc.apply(AddRecord<RegionRecord>{r});
    REQUIRE(result.ok());
    return RegionId(result.allocated);
}

// A closed triangle whose corners are separate points joined by coincidence —
// the shape every tool actually produces, and the one a ring walk matching by
// point identity would get wrong.
struct Triangle {
    Document &doc;
    std::vector<EntityId> edges;

    explicit Triangle(Document &d, double scale = 1.0) : doc(d) {
        const Point corners[3] = {{0.0, 0.0}, {scale * 10.0, 0.0}, {0.0, scale * 10.0}};
        EntityId ends[3][2];
        for(int i = 0; i < 3; i++) {
            const Point a = corners[i];
            const Point b = corners[(i + 1) % 3];
            ends[i][0] = addPoint(doc, a.x, a.y);
            ends[i][1] = addPoint(doc, b.x, b.y);
            edges.push_back(addSegment(doc, ends[i][0], ends[i][1]));
        }
        for(int i = 0; i < 3; i++) {
            addConstraint(doc, ConstraintKind::Coincident,
                          {ends[i][1], ends[(i + 1) % 3][0]});
        }
    }
};

}  // namespace

TEST_CASE("the base layer is implicit and every document has one") {
    // Geometry exists before layers do, so a null LayerId has to mean something
    // rather than nothing: visible, unlocked, and in the order.
    Document doc;
    CHECK(layerVisible(doc, LayerId()));
    CHECK_FALSE(layerLocked(doc, LayerId()));
    CHECK(layerOrder(doc) == std::vector<LayerId>{LayerId()});

    const EntityId p = addPoint(doc, 0.0, 0.0);
    CHECK(isVisible(doc, p));
    CHECK_FALSE(isLocked(doc, p));
}

TEST_CASE("layers stack by order, then by creation") {
    Document doc;
    const LayerId above = addLayer(doc, "above", 5);
    const LayerId below = addLayer(doc, "below", -5);
    const LayerId tieA = addLayer(doc, "tie a", 5);

    // Signed order, so a layer authored later can still sit under the geometry
    // that predates it. Ties break by ID — creation order — so the ordering is
    // total and does not depend on how a sort happened to leave things.
    const std::vector<LayerId> order = layerOrder(doc);
    CHECK(order == std::vector<LayerId>{below, LayerId(), above, tieA});
}

TEST_CASE("hiding and locking are queries about the layer, never about the entity") {
    Document doc;
    const LayerId layer = addLayer(doc, "one", 0);
    EntityRecord p = *doc.entities().find(addPoint(doc, 0.0, 0.0));
    p.layer = layer;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{p}).ok());

    LayerRecord l = *doc.layers().find(layer);
    l.visible = false;
    l.locked = true;
    REQUIRE(doc.apply(SetRecord<LayerRecord>{l}).ok());

    CHECK_FALSE(isVisible(doc, p.id));
    CHECK(isLocked(doc, p.id));
}

TEST_CASE("closure is topological, never visual") {
    // Corners are separate points joined by coincidence, which is what lets a
    // corner be opened by deleting one relation. A ring walk matching endpoints
    // by identity would call every rectangle broken; one matching by
    // coordinates would call an unsolved document broken and flicker.
    Document doc;
    Triangle t(doc);
    const RegionId region = addOutline(doc, t.edges);

    const std::optional<std::vector<BoundaryStep>> ring =
        boundaryRing(doc, *doc.regions().find(region));
    REQUIRE(ring);
    CHECK(ring->size() == 3);
    CHECK(regionState(doc, region) == RegionState::Whole);

    // Move a corner far away without touching a relation. The coincidence still
    // says the joint is one joint, so the region is still whole — the geometry
    // is simply not solved yet.
    EntityRecord corner = *doc.entities().find(ring->front().from);
    corner.seeds[0] += 1000.0;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{corner}).ok());
    CHECK(regionState(doc, region) == RegionState::Whole);

    // Drop the coincidence and the joint comes apart, which is the honest
    // reading: nothing says those two ends are the same vertex any more.
    const ConstraintId first = doc.constraints().records().front().id;
    REQUIRE(doc.apply(RemoveRecord<ConstraintRecord>{first}).ok());
    CHECK(regionState(doc, region) == RegionState::Broken);
}

TEST_CASE("a joint that closes through a point outside the ring is one joint") {
    // The T-junction snap chaining produces whenever a click lands on a spur's
    // endpoint rather than on the corner: the two boundary ends are each
    // coincident with that spur point and only transitively with each other.
    //
    // The document must have one answer about that. findBoundaryCycle walks the
    // whole coincidence partition and calls it closed, so make-solid accepts it;
    // a ring walk that united only coincidences between the ring's own endpoints
    // called it open, and the fill the user had just been given rendered
    // immediately in the broken state. Restricting a transitive closure is not
    // the closure of a restriction.
    Document doc;
    const EntityId a0 = addPoint(doc, 0.0, 0.0);
    const EntityId a1 = addPoint(doc, 10.0, 0.0);
    const EntityId b0 = addPoint(doc, 10.0, 0.0);
    const EntityId b1 = addPoint(doc, 0.0, 10.0);
    const EntityId c0 = addPoint(doc, 0.0, 10.0);
    const EntityId c1 = addPoint(doc, 0.0, 0.0);
    const std::vector<EntityId> edges = {addSegment(doc, a0, a1), addSegment(doc, b0, b1),
                                         addSegment(doc, c0, c1)};

    // Two corners join directly; the third joins through a spur's endpoint,
    // which is on no boundary edge.
    addConstraint(doc, ConstraintKind::Coincident, {a1, b0});
    addConstraint(doc, ConstraintKind::Coincident, {b1, c0});
    const EntityId spur = addPoint(doc, 0.0, 0.0);
    addSegment(doc, spur, addPoint(doc, -5.0, -5.0));
    addConstraint(doc, ConstraintKind::Coincident, {c1, spur});
    addConstraint(doc, ConstraintKind::Coincident, {spur, a0});

    const RegionId region = addOutline(doc, edges);
    const Topology topology(doc);

    // The two definitions of closed, asked side by side. Neither is allowed to
    // be the only one that says yes.
    CHECK(findBoundaryCycle(doc, topology, edges).has_value());
    const std::optional<std::vector<BoundaryStep>> ring =
        boundaryRing(doc, *doc.regions().find(region));
    REQUIRE(ring);
    CHECK(ring->size() == 3);
    CHECK(regionState(doc, region) == RegionState::Whole);
}

TEST_CASE("two straight edges enclose nothing") {
    // They pass the degree test and walk closed, and the 2-gon they report is
    // empty. Harmless while closure only notices; wrong the moment a fill is
    // drawn from it. The bound is about what the edges are rather than how many
    // there are: two curved edges do enclose a lens, and enclosesArea is the one
    // place that distinction lives.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 10.0, 0.0);
    const EntityId a = addSegment(doc, p, q);
    const EntityId b = addSegment(doc, q, p);
    const RegionId region = addOutline(doc, {a, b});
    CHECK_FALSE(boundaryRing(doc, *doc.regions().find(region)));
    CHECK(regionState(doc, region) == RegionState::Broken);
}

TEST_CASE("a composite is exactly as whole as what it combines") {
    Document doc;
    Triangle outer(doc, 3.0);
    Triangle inner(doc, 1.0);
    const RegionId plate = addOutline(doc, outer.edges);
    const RegionId hole = addOutline(doc, inner.edges);

    RegionRecord composite;
    composite.op = CompositeOp::Subtract;
    composite.operands = {plate, hole};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{composite}).ok());
    const RegionId cut = doc.regions().records().back().id;

    CHECK(regionState(doc, cut) == RegionState::Whole);
    // The operands are still live records; they simply are not drawn beside the
    // composite that combines them.
    CHECK(doc.regions().contains(plate));
    CHECK_FALSE(isTopLevelRegion(doc, plate));
    CHECK(regionOrder(doc, LayerId()) == std::vector<RegionId>{cut});

    // Break an operand and the composite goes with it, because a combination
    // with nothing to combine has no area of its own to fall back on.
    RegionRecord broken = *doc.regions().find(hole);
    broken.boundary.pop_back();
    REQUIRE(doc.apply(SetRecord<RegionRecord>{broken}).ok());
    CHECK(regionState(doc, cut) == RegionState::Broken);
    CHECK(brokenRegions(doc) == std::vector<RegionId>{hole, cut});
}

TEST_CASE("z orders within a layer, layers order among themselves") {
    Document doc;
    const LayerId front = addLayer(doc, "front", 1);
    Triangle a(doc), b(doc), c(doc, 2.0);

    const RegionId low = addOutline(doc, a.edges, LayerId(), 0);
    const RegionId high = addOutline(doc, b.edges, LayerId(), 5);
    const RegionId other = addOutline(doc, c.edges, front, -100);

    CHECK(regionOrder(doc, LayerId()) == std::vector<RegionId>{low, high});
    // A region's z never lifts it out of its layer: the layer is the coarse
    // order and z is the fine one, and a huge negative z on a front layer still
    // draws in front.
    CHECK(regionOrder(doc, front) == std::vector<RegionId>{other});
}

TEST_CASE("a tag is broken when it no longer names enough to mean its kind") {
    // A tag owns nothing, so a broken one costs only the affordances it was
    // offering. Every primitive and every surviving constraint is untouched,
    // which is what makes dissolution graceful rather than lossy.
    Document doc;
    Triangle t(doc);
    TagRecord tag;
    tag.kind = TagKind::Rectangle;
    tag.entities = t.edges;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        tag.constraints.push_back(c.id);
    }
    REQUIRE(doc.apply(AddRecord<TagRecord>{tag}).ok());
    // Three edges and three relations is not a rectangle, whatever it says.
    CHECK(tagState(doc, doc.tags().records().front()) == TagState::Broken);
    CHECK(brokenTags(doc).size() == 1);
}

TEST_CASE("the whole composition round-trips through the file") {
    Document doc;
    const LayerId layer = addLayer(doc, "over \"here\"", -3);
    LayerRecord l = *doc.layers().find(layer);
    l.locked = true;
    l.visible = false;
    REQUIRE(doc.apply(SetRecord<LayerRecord>{l}).ok());

    ParameterRecord width;
    width.name = "stroke";
    width.value = Slot(2.5);
    const CommandResult p = doc.apply(AddRecord<ParameterRecord>{width});
    REQUIRE(p.ok());

    StyleRecord style;
    style.name = "ink";
    style.strokeWidth = Slot::parameter(ParameterId(p.allocated));
    style.opacity = Slot(0.5);
    style.filled = true;
    style.fillColor = 0x80112233u;
    const CommandResult s = doc.apply(AddRecord<StyleRecord>{style});
    REQUIRE(s.ok());

    Triangle a(doc), b(doc, 2.0);
    for(EntityId e : a.edges) {
        EntityRecord r = *doc.entities().find(e);
        r.layer = layer;
        r.style = StyleId(s.allocated);
        REQUIRE(doc.apply(SetRecord<EntityRecord>{r}).ok());
    }
    const RegionId one = addOutline(doc, a.edges, layer, 4);
    const RegionId two = addOutline(doc, b.edges, layer, -2);

    RegionRecord punch;
    punch.op = CompositeOp::Intersect;
    punch.operands = {one, two};
    punch.layer = layer;
    punch.z = 7;
    punch.punch = true;
    REQUIRE(doc.apply(AddRecord<RegionRecord>{punch}).ok());

    const std::string text = serialize(doc);
    Document reloaded;
    REQUIRE(deserialize(text, reloaded).ok);
    CHECK(reloaded == doc);
    // A byte fixed point after one save, not a file that shuffles on every one.
    CHECK(serialize(reloaded) == text);
}

TEST_CASE("a plain drawing writes the file it always wrote") {
    // Every field added after the format existed is written only when it is not
    // the default, so gaining an algebra changes no file that does not use one.
    Document doc;
    Triangle t(doc);
    addOutline(doc, t.edges);

    const std::string text = serialize(doc);
    CHECK(text.find(" op=") == std::string::npos);
    CHECK(text.find(" z=") == std::string::npos);
    CHECK(text.find(" punch=") == std::string::npos);
    CHECK(text.find(" opacity=") == std::string::npos);
    // A region line names its operands only when it has some, and an entity
    // line names a style only when one is set. Checked on the lines themselves
    // rather than on the whole file, since a constraint has operands and a
    // region has always named a style.
    CHECK(text.find("region 1 style=0 layer=0 boundary=") != std::string::npos);
    CHECK(text.find("entity 1 kind=point role=normal layer=0 points=") != std::string::npos);
}

TEST_CASE("the bake flattens and reports what it cost") {
    // The one destructive path, and it leads out of the tool. Baking produces a
    // value; the document is not touched, and there is no in-document bake for
    // it to become.
    Document doc;
    Triangle outer(doc, 3.0);
    Triangle inner(doc, 1.0);
    const RegionId plate = addOutline(doc, outer.edges);
    const RegionId hole = addOutline(doc, inner.edges);
    RegionRecord composite;
    composite.op = CompositeOp::Subtract;
    composite.operands = {plate, hole};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{composite}).ok());

    const std::string before = serialize(doc);
    const Bake bake = bakeForExport(doc, Pose(doc));
    CHECK(serialize(doc) == before);

    // One composite, its two operands flattened into rings sharing a group and
    // carrying the operation that joins them. Resolving that operation is the
    // exporter's job, which has a path library; core has no business growing one.
    CHECK(bake.regionsFlattened == 1);
    REQUIRE(bake.fills.size() == 2);
    CHECK(bake.fills[0].group == bake.fills[1].group);
    CHECK(bake.fills[1].combine == CompositeOp::Subtract);
    CHECK(bake.strokes.size() == 6);

    // Honest about being lossy: the constraints do not survive, and the count
    // says so rather than the bake saying nothing.
    CHECK(bake.constraintsDropped == doc.constraints().size());
    CHECK(bake.constraintsDropped > 0);
}

TEST_CASE("the bake keeps the nesting it flattens") {
    // Intersect(A, Union(C, D)) is not A∩C∩D. Tagging every descendant ring with
    // the top composite's operation said it was, and the exporter would have
    // consumed that as truth — so the tree survives as groups naming the group
    // they are an operand of, and the rings arrive in operand order.
    Document doc;
    Triangle a(doc, 1.0), c(doc, 2.0), d(doc, 3.0);
    const RegionId ra = addOutline(doc, a.edges);
    const RegionId rc = addOutline(doc, c.edges);
    const RegionId rd = addOutline(doc, d.edges);

    RegionRecord inner;
    inner.op = CompositeOp::Union;
    inner.operands = {rc, rd};
    const CommandResult innerResult = doc.apply(AddRecord<RegionRecord>{inner});
    REQUIRE(innerResult.ok());

    RegionRecord outer;
    outer.op = CompositeOp::Intersect;
    outer.operands = {ra, RegionId(innerResult.allocated)};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{outer}).ok());

    const Bake bake = bakeForExport(doc, Pose(doc));
    REQUIRE(bake.fills.size() == 3);
    // A meets the union; C and D meet each other.
    CHECK(bake.fills[0].combine == CompositeOp::Intersect);
    CHECK(bake.fills[1].combine == CompositeOp::Union);
    CHECK(bake.fills[2].combine == CompositeOp::Union);
    CHECK(bake.fills[1].group == bake.fills[2].group);
    CHECK(bake.fills[1].group != bake.fills[0].group);
    // And the union is an operand of the intersect rather than a sibling of A.
    CHECK(bake.groups[bake.fills[0].group].parent == NO_BAKE_GROUP);
    CHECK(bake.groups[bake.fills[1].group].parent == bake.fills[0].group);
}

TEST_CASE("a subtract bakes its minuend first") {
    // Nothing in the list says which ring is being cut except its position, so
    // popping operands off a stack in reverse put the subtrahend first and left
    // the exporter to cut the plate out of the hole.
    Document doc;
    Triangle plate(doc, 3.0);
    Triangle hole(doc, 1.0);
    const RegionId minuend = addOutline(doc, plate.edges);
    const RegionId subtrahend = addOutline(doc, hole.edges);
    RegionRecord composite;
    composite.op = CompositeOp::Subtract;
    composite.operands = {minuend, subtrahend};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{composite}).ok());

    const Bake bake = bakeForExport(doc, Pose(doc));
    REQUIRE(bake.fills.size() == 2);
    // The plate is the larger triangle, so its ring reaches further out.
    auto reach = [](const BakedFill &f) {
        double out = 0.0;
        for(const Point &p : f.ring) out = std::max(out, std::hypot(p.x, p.y));
        return out;
    };
    CHECK(reach(bake.fills[0]) > reach(bake.fills[1]));
}

TEST_CASE("the bake tessellates the curves it cannot draw straight") {
    // Circles and arcs are stroked on every screen frame and were absent from
    // the export entirely, with no counter reporting them — a silent loss in the
    // one projection whose whole contract is to say what it destroyed.
    Document doc;
    const EntityId centre = addPoint(doc, 0.0, 0.0);
    addCircle(doc, centre, 5.0);

    const Bake bake = bakeForExport(doc, Pose(doc));
    REQUIRE(bake.strokes.size() > 3);
    for(const BakedStroke &s : bake.strokes) {
        CHECK(std::hypot(s.from.x, s.from.y) == doctest::Approx(5.0));
        CHECK(std::hypot(s.to.x, s.to.y) == doctest::Approx(5.0));
    }
    // A circle's polyline closes on itself; nothing downstream has to guess.
    CHECK(bake.strokes.front().from.x == doctest::Approx(bake.strokes.back().to.x));
    CHECK(bake.strokes.front().from.y == doctest::Approx(bake.strokes.back().to.y));

    // And an arc contributes its sweep and no more: it is open, so the ends
    // stay apart.
    Document arcDoc;
    const EntityId ac = addPoint(arcDoc, 0.0, 0.0);
    const EntityId as = addPoint(arcDoc, 5.0, 0.0);
    const EntityId ae = addPoint(arcDoc, 0.0, 5.0);
    addArc(arcDoc, ac, as, ae);
    const Bake arcBake = bakeForExport(arcDoc, Pose(arcDoc));
    REQUIRE(arcBake.strokes.size() > 3);
    CHECK(arcBake.strokes.front().from.x == doctest::Approx(5.0));
    CHECK(arcBake.strokes.back().to.y == doctest::Approx(5.0));
    // A quarter turn, so a quarter of the circle's runs.
    CHECK(arcBake.strokes.size() < bake.strokes.size());
}

TEST_CASE("a hidden layer is left out of the bake, a locked one is not") {
    // A bake is what the drawing looks like. Locking says nothing about
    // visibility, so locked geometry bakes exactly as ordinary geometry does.
    Document doc;
    const LayerId hidden = addLayer(doc, "hidden", 0);
    const LayerId locked = addLayer(doc, "locked", 1);
    Triangle a(doc), b(doc, 2.0);
    for(EntityId e : a.edges) {
        EntityRecord r = *doc.entities().find(e);
        r.layer = hidden;
        REQUIRE(doc.apply(SetRecord<EntityRecord>{r}).ok());
    }
    for(EntityId e : b.edges) {
        EntityRecord r = *doc.entities().find(e);
        r.layer = locked;
        REQUIRE(doc.apply(SetRecord<EntityRecord>{r}).ok());
    }
    addOutline(doc, a.edges, hidden);
    addOutline(doc, b.edges, locked);

    LayerRecord h = *doc.layers().find(hidden);
    h.visible = false;
    REQUIRE(doc.apply(SetRecord<LayerRecord>{h}).ok());
    LayerRecord k = *doc.layers().find(locked);
    k.locked = true;
    REQUIRE(doc.apply(SetRecord<LayerRecord>{k}).ok());

    const Bake bake = bakeForExport(doc, Pose(doc));
    CHECK(bake.fills.size() == 1);
    CHECK(bake.strokes.size() == 3);
    CHECK(bake.fills.front().layer == locked);
}
