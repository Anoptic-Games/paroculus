// Composition in the hand: lock, hide, group, and degradation through a session.
//
// Layers and groups are organization, and the two couplings that are not are
// the ones asserted hardest here. A locked layer's geometry enters solves
// pinned — not as a Pin constraint, which could over-constrain, but as
// parameters the solver is not solving for. A hidden layer's geometry still
// constrains, and says so whenever it moved something visible.
//
// The degradation half is the flagship deletion story: an edge of a filled loop
// goes, the region renders broken rather than vanishing, and one undo restores
// the bytes.
#include <doctest/doctest.h>

#include <memory>

#include "core/composition.h"
#include "core/persist.h"
#include "interact/registry.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Viewport wideViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

struct Bench {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;

    Bench() {
        session = std::make_unique<Session>(doc, journal);
        session->setViewport(wideViewport());
        session->snapPolicy().gridEnabled = false;
        session->setTool(ToolKind::Select);
    }

    Eigen::Vector2d screen(Point p) const { return session->viewport().view.toScreen(p); }

    void press(Point p) {
        session->handle(PointerEvent::at(PointerAction::Move, screen(p),
                                         session->viewport().view));
        session->handle(PointerEvent::at(PointerAction::Press, screen(p),
                                         session->viewport().view, Button::Left));
    }

    void dragTo(Point from, Point to) {
        press(from);
        session->handle(PointerEvent::at(PointerAction::Move, screen(to),
                                         session->viewport().view, Button::Left));
        session->handle(PointerEvent::at(PointerAction::Release, screen(to),
                                         session->viewport().view, Button::Left));
    }

    LayerId newLayer(const char *name, int32_t order = 0) {
        LayerRecord l;
        l.name = name;
        l.order = order;
        const CommandResult r = doc.apply(AddRecord<LayerRecord>{l});
        REQUIRE(r.ok());
        return LayerId(r.allocated);
    }

    void putOn(LayerId layer, std::vector<EntityId> ids) {
        for(EntityId id : ids) {
            EntityRecord e = *doc.entities().find(id);
            e.layer = layer;
            REQUIRE(doc.apply(SetRecord<EntityRecord>{e}).ok());
        }
        session->refresh();
    }

    void setLayer(LayerId layer, bool visible, bool locked) {
        LayerRecord l = *doc.layers().find(layer);
        l.visible = visible;
        l.locked = locked;
        REQUIRE(doc.apply(SetRecord<LayerRecord>{l}).ok());
        session->refresh();
    }
};

// A segment whose two ends are held apart by a distance, so dragging one end
// has to move the other unless something stops it.
struct Bar {
    EntityId a, b, segment;

    explicit Bar(Document &doc, double y = 0.0) {
        a = addPoint(doc, -50.0, y);
        b = addPoint(doc, 50.0, y);
        segment = addSegment(doc, a, b);
        addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(100.0));
    }
};

}  // namespace

TEST_CASE("a locked layer's parameters come back bit-unchanged") {
    // Lock means "this does not move", which in a solver world means its
    // parameters join the fixed set. Bit-unchanged rather than nearly: a
    // parameter the solver is not solving for is not a parameter it can nudge.
    Bench bench;
    Bar bar(bench.doc);
    const LayerId frozen = bench.newLayer("frozen");
    bench.putOn(frozen, {bar.a});
    bench.setLayer(frozen, true, true);

    const std::optional<Point> before = bench.session->pose().point(bar.a);
    REQUIRE(before);

    // Drag the free end hard. The distance has to give somewhere, and the
    // locked end is not somewhere.
    bench.dragTo(Point{50.0, 0.0}, Point{200.0, 120.0});

    const std::optional<Point> after = bench.session->pose().point(bar.a);
    REQUIRE(after);
    CHECK(after->x == before->x);
    CHECK(after->y == before->y);

    // The seeds in the document are untouched too — nothing was committed on
    // release for the parameter that never moved.
    CHECK(bench.doc.entities().find(bar.a)->seeds[0] == -50.0);
    CHECK(bench.doc.entities().find(bar.a)->seeds[1] == 0.0);
}

TEST_CASE("grabbing locked geometry pins rather than refuses") {
    // The user pulls and nothing moves, which is what a lock is supposed to
    // feel like. A refusal would teach nothing; resistance is discoverable.
    Bench bench;
    Bar bar(bench.doc);
    const LayerId frozen = bench.newLayer("frozen");
    bench.putOn(frozen, {bar.a, bar.b, bar.segment});
    bench.setLayer(frozen, true, true);

    const std::optional<Point> before = bench.session->pose().point(bar.b);
    bench.dragTo(Point{50.0, 0.0}, Point{140.0, 0.0});
    const std::optional<Point> after = bench.session->pose().point(bar.b);
    REQUIRE(before);
    REQUIRE(after);
    CHECK(after->x == before->x);
    CHECK(after->y == before->y);
}

TEST_CASE("locking removes unknowns rather than adding equations") {
    // A pin is a relation the user asked for and can over-constrain; a lock is
    // presentation state and must never be able to make a system inconsistent.
    // So the readout falls and the status stays healthy.
    Bench bench;
    Bar bar(bench.doc);
    bench.session->refresh();
    const int free = bench.session->presentation().dof;
    CHECK(bench.session->presentation().status == SolveStatus::Okay);

    const LayerId frozen = bench.newLayer("frozen");
    bench.putOn(frozen, {bar.a});
    bench.setLayer(frozen, true, true);

    CHECK(bench.session->presentation().dof < free);
    CHECK(bench.session->presentation().status == SolveStatus::Okay);
}

TEST_CASE("locking everything is still a consistent system") {
    // The sharp end of "a lock must never be able to make a system
    // inconsistent". Lock every operand of a relation and it has no unknown
    // left to satisfy it; emitting it anyway hands the solver an equation with
    // nothing to solve for, and the verdict comes back Inconsistent. It is
    // already satisfied by geometry that cannot move, so it goes with them.
    Bench bench;
    Bar bar(bench.doc);
    const LayerId frozen = bench.newLayer("frozen");
    bench.putOn(frozen, {bar.a, bar.b, bar.segment});
    bench.setLayer(frozen, true, true);

    CHECK(bench.session->presentation().status == SolveStatus::Okay);
    CHECK(bench.session->presentation().dof == 0);
    // And the relation is still in the document. Nothing was dropped from the
    // declaration; it was left out of one solve.
    CHECK(bench.doc.constraints().size() == 1);
}

TEST_CASE("hidden still constrains, and says so when it moved something visible") {
    Bench bench;
    Bar bar(bench.doc);
    const LayerId hidden = bench.newLayer("hidden");
    bench.putOn(hidden, {bar.a});
    bench.setLayer(hidden, false, false);

    // The relation is untouched by hiding: the distance still holds, which is
    // the whole difference between hiding a thing and deleting it.
    CHECK(bench.doc.constraints().size() == 1);

    // Move the visible end so the solve has to move the invisible one.
    EntityRecord b = *bench.doc.entities().find(bar.b);
    b.seeds[0] = 200.0;
    REQUIRE(bench.doc.apply(SetRecord<EntityRecord>{b}).ok());
    bench.session->refresh();

    // Both ends move — a seed is a preference and not a pin — but the relation
    // holds, and holding it is what moved the invisible one.
    const std::optional<Point> a = bench.session->pose().point(bar.a);
    const std::optional<Point> b2 = bench.session->pose().point(bar.b);
    REQUIRE(a);
    REQUIRE(b2);
    CHECK(std::abs(std::hypot(a->x - b2->x, a->y - b2->y) - 100.0) < 1e-6);
    CHECK(a->x != -50.0);

    // And the influence is named rather than merely happening.
    CHECK(bench.session->presentation().hiddenInfluences ==
          std::vector<EntityId>{bar.a});
}

TEST_CASE("nothing hidden influences anything when nothing is hidden") {
    // The indication is an event, not a permanent decoration. A document with
    // no hidden layers never emits one however much it solves.
    Bench bench;
    Bar bar(bench.doc);
    EntityRecord b = *bench.doc.entities().find(bar.b);
    b.seeds[0] = 200.0;
    REQUIRE(bench.doc.apply(SetRecord<EntityRecord>{b}).ok());
    bench.session->refresh();
    CHECK(bench.session->presentation().hiddenInfluences.empty());
}

TEST_CASE("hidden geometry cannot be picked or swept up") {
    // Still constraining is not the same as still being there for the hand. A
    // thing the user cannot see is a thing they cannot be aiming at.
    Bench bench;
    Bar bar(bench.doc);
    const LayerId hidden = bench.newLayer("hidden");
    bench.putOn(hidden, {bar.a, bar.b, bar.segment});
    bench.setLayer(hidden, false, false);

    bench.press(Point{-50.0, 0.0});
    CHECK(bench.session->selection().empty());

    CHECK(marquee(bench.session->pose(), bench.session->viewport().view,
                  bench.screen(Point{-200.0, -200.0}), bench.screen(Point{200.0, 200.0}))
              .empty());

    bench.setLayer(hidden, true, false);
    CHECK_FALSE(marquee(bench.session->pose(), bench.session->viewport().view,
                        bench.screen(Point{-200.0, -200.0}),
                        bench.screen(Point{200.0, 200.0}))
                    .empty());
}

TEST_CASE("the partition is layer-blind") {
    // Constraints cross layers freely, so a cross-layer system solves
    // identically to the same system on one layer. If it did not, layers would
    // be semantics rather than organization.
    auto solveOn = [](bool split) {
        Document doc;
        UndoJournal journal;
        Bar bar(doc);
        if(split) {
            LayerRecord l;
            l.name = "other";
            const CommandResult r = doc.apply(AddRecord<LayerRecord>{l});
            REQUIRE(r.ok());
            EntityRecord e = *doc.entities().find(bar.a);
            e.layer = LayerId(r.allocated);
            REQUIRE(doc.apply(SetRecord<EntityRecord>{e}).ok());
        }
        EntityRecord b = *doc.entities().find(bar.b);
        b.seeds[0] = 173.0;
        b.seeds[1] = 61.0;
        REQUIRE(doc.apply(SetRecord<EntityRecord>{b}).ok());

        Session session(doc, journal);
        session.setViewport(wideViewport());
        session.refresh();
        const std::optional<Point> a = session.pose().point(bar.a);
        REQUIRE(a);
        return std::pair<Point, int>{*a, session.presentation().dof};
    };

    const auto together = solveOn(false);
    const auto apart = solveOn(true);
    CHECK(together.first.x == apart.first.x);
    CHECK(together.first.y == apart.first.y);
    CHECK(together.second == apart.second);
}

TEST_CASE("a group drags together") {
    // Drag-together is what a group is for, and it is a default rather than a
    // constraint: the members are held, not bound, so nothing about the
    // document's meaning changed by grouping them.
    Bench bench;
    const EntityId loose = addPoint(bench.doc, 0.0, 80.0);
    Bar bar(bench.doc);
    bench.session->refresh();

    GroupRecord group;
    group.name = "pair";
    group.members = {bar.a, loose};
    REQUIRE(bench.doc.apply(AddRecord<GroupRecord>{group}).ok());
    bench.session->refresh();

    // The group is organization: it added no relation.
    CHECK(bench.doc.constraints().size() == 1);

    const std::optional<Point> before = bench.session->pose().point(loose);
    REQUIRE(before);
    bench.dragTo(Point{-50.0, 0.0}, Point{-50.0, 40.0});
    const std::optional<Point> after = bench.session->pose().point(loose);
    REQUIRE(after);
    // Held, so it stayed with its partner rather than being left behind. A
    // loose point outside the group would not have moved at all: it is not in
    // the dragged component.
    CHECK(after->y != before->y);
}

TEST_CASE("Esc abandons a drag rather than committing it") {
    // Nothing was applied, so there is nothing to undo: the in-flight pose lives
    // in the drag's own solve context and dropping it puts the geometry back.
    // That is the cheapness of a drag read from the other end.
    Bench bench;
    Bar bar(bench.doc);
    bench.session->refresh();
    const std::optional<Point> before = bench.session->pose().point(bar.b);
    REQUIRE(before);
    const bool couldUndo = bench.session->canUndo();

    bench.press(Point{50.0, 0.0});
    bench.session->handle(PointerEvent::at(PointerAction::Move, bench.screen(Point{50.0, 60.0}),
                                           bench.session->viewport().view, Button::Left));
    REQUIRE(bench.session->presentation().dragging);

    bench.session->handle(Key::Escape);
    CHECK_FALSE(bench.session->presentation().dragging);
    const std::optional<Point> after = bench.session->pose().point(bar.b);
    REQUIRE(after);
    CHECK(after->x == before->x);
    CHECK(after->y == before->y);
    // Nothing to take back, because nothing was applied.
    CHECK(bench.session->canUndo() == couldUndo);

    // And the abandoned gesture does not come back as a marquee on the way out:
    // the press flags went with the drag.
    bench.session->handle(PointerEvent::at(PointerAction::Move, bench.screen(Point{80.0, 80.0}),
                                           bench.session->viewport().view, Button::Left));
    CHECK_FALSE(bench.session->presentation().marqueeActive);
    bench.session->handle(PointerEvent::at(PointerAction::Release,
                                           bench.screen(Point{80.0, 80.0}),
                                           bench.session->viewport().view, Button::Left));
    CHECK(bench.session->canUndo() == couldUndo);
}

TEST_CASE("promoting a measurement to driving is checked like the imposition it is") {
    // Consistency cannot break — the value is captured from the pose, so it
    // holds where the geometry already is — but redundancy can, and redundancy
    // is where later edits go to die. The imposition path raises that flag for
    // the identical declaration; a toggle that skipped it was a quiet way to
    // plant one.
    Bench bench;
    Bar bar(bench.doc);
    // A second distance saying the same thing as the one Bar already imposed,
    // recorded as a measurement so it drives nothing yet.
    ConstraintRecord measurement;
    measurement.kind = ConstraintKind::PointPointDistance;
    measurement.operands = {bar.a, bar.b, EntityId(), EntityId()};
    measurement.value = Slot(100.0);
    measurement.driving = false;
    const CommandResult added = bench.doc.apply(AddRecord<ConstraintRecord>{measurement});
    REQUIRE(added.ok());
    bench.session->refresh();

    bench.session->selectConstraint(ConstraintId(added.allocated));
    REQUIRE(invokeAction(*bench.session, "relation.toggle-driving"));
    CHECK(bench.doc.constraints().find(ConstraintId(added.allocated))->driving);
    // Flagged, not refused: the toggle is the user's to make and a redundant
    // relation is a warning they are entitled to rather than a fault today.
    CHECK(bench.session->presentation().impositionVerdict == CandidateVerdict::Redundant);
}

TEST_CASE("a hidden layer's marks go with its geometry") {
    // Hiding removed the geometry from the canvas, from hit testing and from
    // the marquee, and left its relations' marks drawn — floating over empty
    // space, still clickable, selecting relations on geometry the user could
    // neither see nor pick, and spending slots in the glyph budget to do it.
    Bench bench;
    Bar bar(bench.doc);
    addConstraint(bench.doc, ConstraintKind::Horizontal, {bar.segment});
    bench.session->refresh();
    const size_t visible = bench.session->glyphs().size();
    REQUIRE(visible > 0);

    const LayerId hidden = bench.newLayer("hidden");
    bench.putOn(hidden, {bar.a, bar.b, bar.segment});
    bench.setLayer(hidden, false, false);
    CHECK(bench.session->glyphs().empty());

    // Showing it again brings them back: nothing was dropped from the document,
    // and the marks are a view of it like everything else on the canvas.
    bench.setLayer(hidden, true, false);
    CHECK(bench.session->glyphs().size() == visible);
}

TEST_CASE("a relation across a hidden layer still marks the half that shows") {
    // The other side of the bargain. A mark goes where its operand is, so a
    // hidden operand has nowhere honest for one — but the constraint is not
    // hidden, and the visible operand it binds still says so. That a hidden
    // operand is constraining is what the influence indication is for.
    Bench bench;
    Bar first(bench.doc, 0.0);
    Bar second(bench.doc, 60.0);
    const ConstraintId parallel =
        addConstraint(bench.doc, ConstraintKind::Parallel, {first.segment, second.segment});
    bench.session->refresh();

    auto marksOn = [&](ConstraintId id) {
        std::vector<GlyphMark> out;
        for(const GlyphMark &m : bench.session->glyphs()) {
            if(m.constraint == id) out.push_back(m);
        }
        return out;
    };
    // One mark per operand, which is what makes a parallel visible from both
    // segments rather than from one.
    REQUIRE(marksOn(parallel).size() == 2);

    const LayerId hidden = bench.newLayer("hidden");
    bench.putOn(hidden, {first.a, first.b, first.segment});
    bench.setLayer(hidden, false, false);

    const std::vector<GlyphMark> marks = marksOn(parallel);
    REQUIRE(marks.size() == 1);
    CHECK(marks.front().on == second.segment);
}

TEST_CASE("a group carries what it can move and leaves a lock where it is") {
    // A carry writes seeds outside the solve, and a locked parameter's seed is
    // its known value — so a carry that did not check would not ask for a move,
    // it would perform one and commit it. seedTarget refuses exactly this write
    // for the grab; the carry has to refuse it too, or a lock is pinned against
    // the hand and not against a group.
    Bench bench;
    const EntityId pinned = addPoint(bench.doc, 0.0, 80.0);
    Bar bar(bench.doc);
    const LayerId frozen = bench.newLayer("frozen");
    bench.putOn(frozen, {pinned});
    bench.setLayer(frozen, true, true);

    GroupRecord group;
    group.name = "pair";
    group.members = {bar.a, pinned};
    REQUIRE(bench.doc.apply(AddRecord<GroupRecord>{group}).ok());
    bench.session->refresh();

    const std::optional<Point> before = bench.session->pose().point(pinned);
    REQUIRE(before);
    bench.dragTo(Point{-50.0, 0.0}, Point{-50.0, 40.0});

    const std::optional<Point> after = bench.session->pose().point(pinned);
    REQUIRE(after);
    CHECK(after->x == before->x);
    CHECK(after->y == before->y);
    // And nothing was committed for it either: the seeds are the seeds.
    CHECK(bench.doc.entities().find(pinned)->seeds[0] == 0.0);
    CHECK(bench.doc.entities().find(pinned)->seeds[1] == 80.0);
}

TEST_CASE("a fill lands on the layer of the outline that defines it") {
    // Taking the lowest-ID layer record put every fill on whatever layer existed
    // first, so hiding or locking the layer the outline was drawn on split the
    // fill from the geometry that defines it and the fill competed for z-order
    // in the wrong layer's stack. The corpus could not see it because every
    // corpus fill happens before any layer exists.
    Bench bench;
    const LayerId drawn = bench.newLayer("drawn", 3);
    bench.session->setTool(ToolKind::Line);
    bench.press(Point{-60.0, -40.0});
    bench.press(Point{60.0, -40.0});
    bench.press(Point{60.0, 40.0});
    bench.press(Point{-60.0, 40.0});
    bench.press(Point{-59.5, -39.5});
    bench.session->setTool(ToolKind::Select);

    // Everything the square is made of goes to the named layer.
    std::vector<EntityId> all;
    for(const EntityRecord &e : bench.doc.entities().records()) all.push_back(e.id);
    bench.putOn(drawn, all);

    bench.press(Point{0.0, -40.0});
    REQUIRE(invokeAction(*bench.session, "region.make-solid"));
    const RegionId region = bench.session->presentation().filled;
    REQUIRE(region.valid());
    CHECK(bench.doc.regions().find(region)->layer == drawn);
    // And no style it never asked for: a fill nobody styled reads as the outline
    // it belongs to.
    CHECK_FALSE(bench.doc.regions().find(region)->style.valid());
    CHECK(regionOrder(bench.doc, drawn) == std::vector<RegionId>{region});
    CHECK(regionOrder(bench.doc, LayerId()).empty());
}

TEST_CASE("subtract cuts the upper region out of the lower one") {
    // Which region a subtract subtracts from is a role ambiguity in exactly the
    // sense length-ratio gets a surface for, and it was answered by creation
    // order — which correlates with nothing the user can see. The occlusion
    // order does, and the action takes an argument for the other reading.
    Bench bench;

    auto square = [&](double cx, double cy, double half) {
        bench.session->setTool(ToolKind::Line);
        bench.press(Point{cx - half, cy - half});
        bench.press(Point{cx + half, cy - half});
        bench.press(Point{cx + half, cy + half});
        bench.press(Point{cx - half, cy + half});
        bench.press(Point{cx - half + 0.5, cy - half + 0.5});
        bench.session->setTool(ToolKind::Select);
        bench.press(Point{cx, cy - half});
        REQUIRE(invokeAction(*bench.session, "region.make-solid"));
        return bench.session->presentation().filled;
    };

    const RegionId plate = square(0.0, 0.0, 60.0);
    const RegionId disc = square(200.0, 0.0, 20.0);
    REQUIRE(plate.valid());
    REQUIRE(disc.valid());

    // The disc sits above the plate, which is what the user is looking at.
    RegionRecord raised = *bench.doc.regions().find(disc);
    raised.z = 4;
    REQUIRE(bench.doc.apply(SetRecord<RegionRecord>{raised}).ok());

    std::vector<EntityId> both = bench.doc.regions().find(plate)->boundary;
    for(EntityId e : bench.doc.regions().find(disc)->boundary) both.push_back(e);
    bench.session->select(both);
    REQUIRE(invokeAction(*bench.session, "region.subtract"));

    const RegionId cut = bench.session->presentation().composed;
    REQUIRE(cut.valid());
    CHECK(bench.doc.regions().find(cut)->operands == std::vector<RegionId>{plate, disc});

    // Put the disc underneath instead and the reading follows what is on screen
    // rather than which region was drawn first.
    bench.session->select(both);
    REQUIRE(invokeAction(*bench.session, "region.lift"));
    RegionRecord lowered = *bench.doc.regions().find(disc);
    lowered.z = -4;
    REQUIRE(bench.doc.apply(SetRecord<RegionRecord>{lowered}).ok());
    bench.session->select(both);
    REQUIRE(invokeAction(*bench.session, "region.subtract"));
    const RegionId second = bench.session->presentation().composed;
    REQUIRE(second.valid());
    CHECK(bench.doc.regions().find(second)->operands == std::vector<RegionId>{disc, plate});

    // And the other reading is sayable, which is the half a default cannot be.
    bench.session->select(both);
    REQUIRE(invokeAction(*bench.session, "region.lift"));
    bench.session->select(both);
    ActionArguments reversed;
    reversed.set("order", 1.0);
    REQUIRE(invokeAction(*bench.session, "region.subtract", reversed));
    const RegionId third = bench.session->presentation().composed;
    REQUIRE(third.valid());
    CHECK(bench.doc.regions().find(third)->operands == std::vector<RegionId>{plate, disc});
}

TEST_CASE("deleting an edge of a filled loop degrades the region, and undo restores it") {
    // The flagship deletion story. The fill is not blocked, not silently
    // discarded, and not left showing an area the document no longer bounds.
    Bench bench;
    bench.session->setTool(ToolKind::Line);
    bench.press(Point{-60.0, -40.0});
    bench.press(Point{60.0, -40.0});
    bench.press(Point{60.0, 40.0});
    bench.press(Point{-60.0, 40.0});
    bench.press(Point{-59.5, -39.5});
    bench.session->setTool(ToolKind::Select);
    bench.press(Point{0.0, -40.0});
    REQUIRE(invokeAction(*bench.session, "region.make-solid"));

    const RegionId region = bench.session->presentation().filled;
    REQUIRE(region.valid());
    CHECK(regionState(bench.doc, region) == RegionState::Whole);
    const std::string whole = serialize(bench.doc);

    // Take one edge out from under it.
    bench.press(Point{0.0, -40.0});
    REQUIRE_FALSE(bench.session->selection().empty());
    bench.session->handle(Key::Delete);

    // The record is still there, still saying what it meant, and now says it
    // cannot mean it.
    REQUIRE(bench.doc.regions().contains(region));
    CHECK(regionState(bench.doc, region) == RegionState::Broken);
    CHECK(bench.session->presentation().brokenRegions == std::vector<RegionId>{region});
    // Reported as degraded rather than as deleted: it is still on screen.
    CHECK(bench.session->presentation().degraded > 0);

    // One step back, and the bytes are the bytes.
    bench.session->handle(Key::Undo);
    bench.session->refresh();
    CHECK(serialize(bench.doc) == whole);
    CHECK(regionState(bench.doc, region) == RegionState::Whole);
    CHECK(bench.session->presentation().brokenRegions.empty());
}

TEST_CASE("composing two fills consumes neither, and lifting undoes it exactly") {
    Bench bench;

    auto square = [&](double cx, double cy, double half) {
        bench.session->setTool(ToolKind::Line);
        bench.press(Point{cx - half, cy - half});
        bench.press(Point{cx + half, cy - half});
        bench.press(Point{cx + half, cy + half});
        bench.press(Point{cx - half, cy + half});
        bench.press(Point{cx - half + 0.5, cy - half + 0.5});
        bench.session->setTool(ToolKind::Select);
        bench.press(Point{cx, cy - half});
        REQUIRE(invokeAction(*bench.session, "region.make-solid"));
        return bench.session->presentation().filled;
    };

    const RegionId plate = square(0.0, 0.0, 60.0);
    const RegionId hole = square(200.0, 0.0, 20.0);
    REQUIRE(plate.valid());
    REQUIRE(hole.valid());

    // A region is named by selecting what bounds it: it has no handle of its
    // own, which is the same reason it has no geometry of its own.
    std::vector<EntityId> both = bench.doc.regions().find(plate)->boundary;
    for(EntityId e : bench.doc.regions().find(hole)->boundary) both.push_back(e);
    bench.session->select(both);
    CHECK(bench.session->selectedRegions().size() == 2);

    const std::string before = serialize(bench.doc);
    REQUIRE(invokeAction(*bench.session, "region.subtract"));

    const RegionId cut = bench.session->presentation().composed;
    REQUIRE(cut.valid());
    CHECK(bench.doc.regions().find(cut)->op == CompositeOp::Subtract);
    // Nothing was consumed. Both operands are exactly the records they were.
    CHECK(bench.doc.regions().contains(plate));
    CHECK(bench.doc.regions().contains(hole));
    CHECK(regionOrder(bench.doc, LayerId()) == std::vector<RegionId>{cut});

    // And lifting is a real inverse, not a partial one. Record content rather
    // than bytes, because the composite spent a region ID and a watermark is
    // never rewound — deliberately, since the redo record still names it.
    bench.session->select(both);
    REQUIRE(invokeAction(*bench.session, "region.lift"));
    Document was;
    REQUIRE(deserialize(before, was).ok);
    CHECK(sameRecords(bench.doc, was));
    CHECK(regionOrder(bench.doc, LayerId()) == std::vector<RegionId>{plate, hole});
}
