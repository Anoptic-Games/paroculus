// The flagship: segments to solid.
//
// A user draws four segments; endpoint snapping makes the joints coincident as
// they draw; the loop closes. Making it a solid is not a conversion — a region
// record references the cycle, no geometry is copied, no constraint is touched
// — so dragging a vertex moves the fill, because the fill has no geometry of
// its own to go stale.
//
// This is the whole thesis in one gesture, so it is asserted here as a scripted
// session rather than as a set of unit checks: the point is that the sequence
// works end to end through the same code the pointer drives, and that the file
// it produces can be replayed and watched. Watching is a different instrument
// from checking, and a state can satisfy every invariant on the list and still
// be visibly wrong.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

#include "core/persist.h"
#include "interact/registry.h"
#include "interact/script.h"
#include "interact/session.h"
#include "render/view.h"
#include "support/build.h"

using namespace paroculus;

namespace {

constexpr int W = 800;
constexpr int H = 600;
constexpr uint32_t BACKGROUND = 0xff14161au;

Viewport flagshipViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(W * 0.5, H * 0.5));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = W;
    v.height = H;
    return v;
}

// Whether the pixel at a document point is anything but background.
bool painted(const Pose &pose, const ViewTransform &view, Point p) {
    std::vector<uint32_t> pixels(static_cast<size_t>(W) * H, 0u);
    renderDocument(pose, view, Adornment{}, reinterpret_cast<uint8_t *>(pixels.data()), W, H,
                   static_cast<size_t>(W) * 4);
    const Eigen::Vector2d s = view.toScreen(p);
    const int x = static_cast<int>(s.x());
    const int y = static_cast<int>(s.y());
    if(x < 0 || y < 0 || x >= W || y >= H) return false;
    return pixels[static_cast<size_t>(y) * W + x] != BACKGROUND;
}

// The whole gesture, as steps. Written as a script rather than driven directly
// so the corpus entry and the `--script` artefact are the same thing.
GestureScript flagshipScript() {
    const Viewport viewport = flagshipViewport();
    GestureScript script;

    auto push = [&](ScriptStep step) {
        step.modifiers = Modifier::None;
        script.steps.push_back(std::move(step));
    };

    ScriptStep view;
    view.kind = ScriptStep::Kind::Viewport;
    view.viewport = viewport;
    push(view);

    ScriptStep tool;
    tool.kind = ScriptStep::Kind::Tool;
    tool.tool = ToolKind::Line;
    push(tool);

    auto click = [&](Point p) {
        const Eigen::Vector2d s = viewport.view.toScreen(p);
        ScriptStep move;
        move.kind = ScriptStep::Kind::Pointer;
        move.action = PointerAction::Move;
        move.screen = s;
        push(move);

        ScriptStep press;
        press.kind = ScriptStep::Kind::Pointer;
        press.action = PointerAction::Press;
        press.button = Button::Left;
        press.screen = s;
        press.clicks = 1;
        push(press);
    };

    // Four corners, drawn a little sloppily. Inference squares it up as it goes
    // — that is stage 4's thesis working, and the outline this stage fills.
    click(Point{-120.0, -80.0});
    click(Point{120.0, -79.0});
    click(Point{119.0, 80.0});
    click(Point{-120.0, 79.0});
    // Back to the start, near enough for endpoint snapping to close the loop.
    click(Point{-119.0, -79.0});

    // Home, then click the outline. Leaving a creation tool drops the offers
    // that were about the placement in flight, so back in selection the fill
    // offer is about what is selected — which is the same rule every other
    // contextual offer follows, and the reason the strip is a projection of the
    // selection rather than a memory of the last thing drawn.
    ScriptStep select;
    select.kind = ScriptStep::Kind::Tool;
    select.tool = ToolKind::Select;
    push(select);
    click(Point{0.0, -80.0});

    ScriptStep solid;
    solid.kind = ScriptStep::Kind::Action;
    solid.actionName = "region.make-solid";
    push(solid);

    return script;
}

}  // namespace

TEST_CASE("flagship: draw an outline, make it solid, drag a vertex, the fill follows") {
    const GestureScript script = flagshipScript();
    Document doc = script.document;
    UndoJournal journal;
    Session session(doc, journal);
    replay(session, script);

    // The outline closed and was filled, and filling touched nothing else.
    REQUIRE(doc.regions().records().size() == 1);
    const RegionRecord region = doc.regions().records().front();
    REQUIRE(region.boundary.size() == 4);

    const size_t entities = doc.entities().records().size();
    const size_t relations = doc.constraints().records().size();

    // Inference squared the sloppy drawing up: the edges are axis-aligned even
    // though no click was.
    size_t axisRelations = 0;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind == ConstraintKind::Horizontal || c.kind == ConstraintKind::Vertical) {
            axisRelations++;
        }
    }
    CHECK(axisRelations >= 3);

    // The fill is where the outline is.
    const ViewTransform &view = session.viewport().view;
    CHECK(painted(session.pose(), view, Point{0.0, 0.0}));
    CHECK_FALSE(painted(session.pose(), view, Point{-250.0, 0.0}));

    // Now drag one corner outward, holding only that corner so the rest can
    // give. A drag holds every selected parameter; selecting the whole square
    // would be asking a rigid body to deform.
    const EntityRecord *edge = doc.entities().find(region.boundary.front());
    REQUIRE(edge != nullptr);
    const EntityId corner = edge->points[0];
    const Point was = *session.pose().point(corner);
    session.select({corner});

    const Eigen::Vector2d from = view.toScreen(was);
    const Eigen::Vector2d to = view.toScreen(Point{was.x - 90.0, was.y});
    session.handle(PointerEvent::at(PointerAction::Press, from, view, Button::Left));
    for(int i = 1; i <= 10; i++) {
        session.handle(PointerEvent::at(PointerAction::Move, from + (to - from) * i / 10.0,
                                        view, Button::Left));
    }
    session.handle(PointerEvent::at(PointerAction::Release, to, view, Button::Left));

    const Point now = *session.pose().point(corner);
    CHECK(std::fabs(now.x - was.x) > 20.0);

    // And the fill followed, into territory that was background before the
    // drag. Nothing was told to update it, because there is nothing to update:
    // the path is walked from the pose every frame.
    CHECK(painted(session.pose(), view, Point{-180.0, 0.0}));

    // Filling and dragging added no geometry and dropped no relation. The
    // outline kept operating throughout, which is the whole claim.
    CHECK(doc.entities().records().size() == entities);
    CHECK(doc.constraints().records().size() == relations);
    CHECK(doc.regions().records().front().boundary == region.boundary);
}

TEST_CASE("flagship: the region is exactly one undoable record") {
    const GestureScript script = flagshipScript();
    Document doc = script.document;
    UndoJournal journal;
    Session session(doc, journal);

    // Everything but the fill.
    GestureScript outline = script;
    outline.steps.pop_back();
    replay(session, outline);
    const std::string before = serialize(doc);
    REQUIRE(doc.regions().records().empty());

    REQUIRE(invokeAction(session, "region.make-solid"));
    REQUIRE(doc.regions().records().size() == 1);

    // The inverse is deleting that record. The outline and every constraint
    // survive untouched, and round-tripping is exact because nothing was
    // translated in either direction.
    //
    // Record equality rather than byte equality, and the difference is one
    // watermark: undo restores every record exactly but never rewinds an ID
    // counter, because the redo record still names the ID the undone add took
    // and reissuing it would rebind that reference to a different object.
    REQUIRE(invokeAction(session, "edit.undo"));
    Document outlineOnly;
    REQUIRE(deserialize(before, outlineOnly).ok);
    CHECK(sameRecords(doc, outlineOnly));
    CHECK(doc.regions().records().empty());
}

TEST_CASE("flagship: the script round-trips through the file format") {
    // A script is a recording, not a description. The corpus entry above and
    // the artefact a person plays back with `--script` have to be the same
    // thing, or watching would be watching something else.
    const GestureScript script = flagshipScript();
    const std::string text = serializeScript(script);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    CHECK(serializeScript(parsed) == text);

    // And replaying the parsed one reaches the same document as replaying the
    // original, byte for byte.
    auto play = [](const GestureScript &s) {
        Document doc = s.document;
        UndoJournal journal;
        Session session(doc, journal);
        replay(session, s);
        return serialize(doc);
    };
    CHECK(play(parsed) == play(script));
}

TEST_CASE("flagship: the checked-in script is the one the corpus asserts") {
    // The corpus entry and the artefact a person plays back with `--script` are
    // the same session or the watching proves nothing about the checking. This
    // is what keeps them the same: the file on disk must be byte-identical to
    // what the builder above produces, so a change to the gesture fails here
    // rather than quietly leaving a stale demo in the repository.
    const std::string path = std::string(PAROCULUS_CORPUS_DIR) + "/flagship.paro";
    std::ifstream file(path);
    REQUIRE_MESSAGE(file.good(), path);
    std::ostringstream buffer;
    buffer << file.rdbuf();

    CHECK(buffer.str() == serializeScript(flagshipScript()));

    // And it plays back to a filled outline, which is what `--script` shows.
    GestureScript parsed;
    REQUIRE(parseScript(buffer.str(), parsed).ok);
    Document doc = parsed.document;
    UndoJournal journal;
    Session session(doc, journal);
    replay(session, parsed);
    CHECK(doc.regions().records().size() == 1);
}

TEST_CASE("flagship: recording the replay reproduces the script") {
    // Record → replay → record is the identity the format exists to guarantee,
    // and the fill action is the first edit with no other recording surface
    // behind it: every earlier action dispatched to a pointer event, a
    // keystroke or a tool change, each of which records itself, so an action
    // step re-recorded as the change it caused. Making a solid causes no such
    // input, so it records itself — and this is what says so.
    const GestureScript script = flagshipScript();
    const std::string text = serializeScript(script);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);

    Document doc = parsed.document;
    UndoJournal journal;
    Session session(doc, journal);
    ScriptRecorder again;
    session.setRecorder(&again);
    replay(session, parsed);

    GestureScript second;
    second.document = parsed.document;
    second.steps = again.steps();
    CHECK(serializeScript(second) == text);

    // And the action really is in there, rather than the identity holding
    // because both sides dropped it.
    size_t actions = 0;
    for(const ScriptStep &step : again.steps()) {
        if(step.kind == ScriptStep::Kind::Action) actions++;
    }
    CHECK(actions == 1);
}
