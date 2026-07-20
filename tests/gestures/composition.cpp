// The composition, driven as a session and replayed as a script.
//
// Every stage-6 action reaches the document through the registry, so the corpus
// question is the same one the flagship asked of make-solid: does a gesture that
// hides a layer, locks one, groups geometry and cuts one fill out of another
// record itself, replay identically, and re-record byte for byte.
//
// It matters more here than usual. None of these actions has another recording
// surface behind it — no pointer event, no keystroke, no tool change — so each
// one either writes its own step or vanishes from every script that used it.
#include <doctest/doctest.h>

#include <string>

#include "core/composition.h"
#include "core/persist.h"
#include "interact/registry.h"
#include "interact/script.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Viewport benchViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

void press(Session &session, Point p, Modifier modifiers = Modifier::None) {
    const Eigen::Vector2d s = session.viewport().view.toScreen(p);
    session.handle(PointerEvent::at(PointerAction::Move, s, session.viewport().view));
    PointerEvent event =
        PointerEvent::at(PointerAction::Press, s, session.viewport().view, Button::Left);
    event.modifiers = modifiers;
    session.handle(event);
}

// Draws a closed square and fills it. Clicking the outline afterwards is what a
// user does and what the script replays: a region has no handle of its own, so
// it is named by selecting the run that bounds it.
void fillSquare(Session &session, double cx, double cy, double half) {
    session.setTool(ToolKind::Line);
    press(session, Point{cx - half, cy - half});
    press(session, Point{cx + half, cy - half});
    press(session, Point{cx + half, cy + half});
    press(session, Point{cx - half, cy + half});
    press(session, Point{cx - half + 0.5, cy - half + 0.5});
    session.setTool(ToolKind::Select);
    press(session, Point{cx, cy - half});
    REQUIRE(invokeAction(session, "region.make-solid"));
}

// The whole composition gesture, recorded. Two fills, one cut out of the other,
// a group, and a layer that ends up locked and hidden.
//
// Every selection is made by clicking, never by calling Session::select. That
// is not fastidiousness: select is deliberately not a recording surface — what
// a script records is the click — so a gesture built the other way replays
// without a selection and every action in it quietly refuses.
GestureScript compositionScript() {
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(benchViewport());
    session.snapPolicy().gridEnabled = false;

    ScriptRecorder recorder;
    session.setRecorder(&recorder);

    fillSquare(session, 0.0, 0.0, 60.0);
    fillSquare(session, 200.0, 0.0, 20.0);

    // A marquee takes what it wholly contains, which is how two fills are named
    // at once: a shift-click adds the one entity under it, and a region is only
    // named when everything bounding it is selected.
    auto selectBoth = [&] {
        const ViewTransform &view = session.viewport().view;
        const Eigen::Vector2d from = view.toScreen(Point{-100.0, -100.0});
        const Eigen::Vector2d to = view.toScreen(Point{250.0, 100.0});
        session.handle(PointerEvent::at(PointerAction::Move, from, view));
        session.handle(PointerEvent::at(PointerAction::Press, from, view, Button::Left));
        session.handle(PointerEvent::at(PointerAction::Move, to, view, Button::Left));
        session.handle(PointerEvent::at(PointerAction::Release, to, view, Button::Left));
    };

    selectBoth();
    REQUIRE(invokeAction(session, "region.subtract"));

    // Empty space first: clicking inside a selection keeps it, which is what
    // makes a multi-selection draggable as one thing and what would otherwise
    // group both squares here.
    press(session, Point{-250.0, 200.0});
    press(session, Point{200.0, -20.0});
    REQUIRE(invokeAction(session, "group.create"));

    REQUIRE(invokeAction(session, "layer.new"));
    selectBoth();
    REQUIRE(invokeAction(session, "layer.assign"));
    REQUIRE(invokeAction(session, "layer.lock"));
    REQUIRE(invokeAction(session, "layer.hide"));

    GestureScript script;
    script.steps = recorder.steps();
    return script;
}

}  // namespace

TEST_CASE("composition: the gesture reaches the document it describes") {
    const GestureScript script = compositionScript();

    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(benchViewport());
    session.snapPolicy().gridEnabled = false;
    replay(session, script);

    // Two outlines, one composite over them. Nothing was consumed to make it.
    REQUIRE(doc.regions().size() == 3);
    const RegionRecord &composite = doc.regions().records().back();
    CHECK(composite.op == CompositeOp::Subtract);
    CHECK(composite.operands.size() == 2);
    for(RegionId id : composite.operands) CHECK(doc.regions().contains(id));

    // One layer, holding everything, hidden and locked.
    REQUIRE(doc.layers().size() == 1);
    const LayerRecord &layer = doc.layers().records().front();
    CHECK_FALSE(layer.visible);
    CHECK(layer.locked);

    // And one group over the connected run the click selected — four edges and
    // the five points they are drawn through. It added no relation, because
    // membership is organization and nothing reads it as structure.
    REQUIRE(doc.groups().size() == 1);
    CHECK(doc.groups().records().front().members.size() == 9);

    // Locked geometry is fixed and hidden geometry still constrains, so the
    // document solves without complaint and every relation is still here.
    session.refresh();
    CHECK(session.presentation().status == SolveStatus::Okay);
    CHECK(doc.constraints().size() > 0);
}

TEST_CASE("composition: recording the replay reproduces the script") {
    // Every composition action writes its own step, because nothing else will.
    // If one did not, this would still pass with both sides silently short —
    // which is what the action count below is for.
    const GestureScript script = compositionScript();
    const std::string text = serializeScript(script);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);

    Document doc = parsed.document;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(benchViewport());
    session.snapPolicy().gridEnabled = false;
    ScriptRecorder again;
    session.setRecorder(&again);
    replay(session, parsed);

    GestureScript second;
    second.document = parsed.document;
    second.steps = again.steps();
    CHECK(serializeScript(second) == text);

    size_t composition = 0;
    for(const ScriptStep &step : again.steps()) {
        if(step.kind != ScriptStep::Kind::Action) continue;
        // Every recorded step names an action the registry has. A step naming
        // something it does not is a step replay silently drops: the script
        // parses, the edit is gone, and the identity above still holds because
        // both sides lost it.
        CHECK(findAction(step.actionName) != nullptr);
        if(step.actionName.rfind("layer.", 0) == 0 || step.actionName.rfind("group.", 0) == 0 ||
           step.actionName == "region.subtract") {
            composition++;
        }
    }
    CHECK(composition == 6);
}

TEST_CASE("composition: the whole gesture round-trips through the file") {
    // The script reaches a document, and the document reaches a file and back.
    // Layers, styles, z-order and the algebra are all authoring intent, so they
    // are all in the file, and a save is a byte fixed point after one write.
    const GestureScript script = compositionScript();

    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(benchViewport());
    session.snapPolicy().gridEnabled = false;
    replay(session, script);

    const std::string text = serialize(doc);
    Document reloaded;
    REQUIRE(deserialize(text, reloaded).ok);
    CHECK(reloaded == doc);
    CHECK(serialize(reloaded) == text);
}

TEST_CASE("composition: undo walks back out of it one step at a time") {
    // Each action is one gesture and therefore one undo step. Nothing here
    // bundles two edits into a step the user cannot take apart, and nothing
    // leaves a half-applied composition behind.
    const GestureScript script = compositionScript();

    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(benchViewport());
    session.snapPolicy().gridEnabled = false;
    replay(session, script);

    REQUIRE(doc.layers().size() == 1);
    session.handle(Key::Undo);  // hide
    CHECK(doc.layers().records().front().visible);
    session.handle(Key::Undo);  // lock
    CHECK_FALSE(doc.layers().records().front().locked);
    session.handle(Key::Undo);  // assign
    CHECK_FALSE(doc.entities().records().front().layer.valid());
    session.handle(Key::Undo);  // new layer
    CHECK(doc.layers().empty());
    session.handle(Key::Undo);  // group
    CHECK(doc.groups().empty());
    session.handle(Key::Undo);  // subtract

    // Back to two ordinary fills, both drawn in their own right again.
    CHECK(doc.regions().size() == 2);
    CHECK(regionOrder(doc, LayerId()).size() == 2);
    for(const RegionRecord &r : doc.regions().records()) {
        CHECK(r.op == CompositeOp::Outline);
        CHECK(regionState(doc, r) == RegionState::Whole);
    }
}
