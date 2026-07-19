#include <doctest/doctest.h>

#include "core/persist.h"
#include "interact/script.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;

namespace {

Viewport toolViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

struct Fixture {
    Document doc;
    UndoJournal journal;
};

// Clicks at a document position, which is what a creation tool consumes.
void clickAt(Session &session, Point p) {
    const Eigen::Vector2d screen = session.viewport().view.toScreen(p);
    const Viewport &v = session.viewport();
    session.handle(PointerEvent::at(PointerAction::Press, screen, v.view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, screen, v.view, Button::Left));
}

void moveTo(Session &session, Point p) {
    const Eigen::Vector2d screen = session.viewport().view.toScreen(p);
    session.handle(PointerEvent::at(PointerAction::Move, screen, session.viewport().view));
}

size_t countOf(const Document &doc, EntityKind kind) {
    size_t n = 0;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind == kind) n++;
    }
    return n;
}

}  // namespace

TEST_CASE("the first click of a chain commits nothing") {
    // A click the user abandons has to leave the document exactly as it was,
    // and a lone floating point is not that.
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    const std::string before = serialize(f.doc);

    session.setTool(ToolKind::Line);
    clickAt(session, Point{0.0, 0.0});

    CHECK(serialize(f.doc) == before);
    CHECK_FALSE(f.journal.canUndo());
    // But the rubber band is live, so the user can see the tool is armed.
    CHECK(session.presentation().toolPreview.active);
}

TEST_CASE("a second click commits one segment as one undo step") {
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    session.setTool(ToolKind::Line);

    clickAt(session, Point{-40.0, 0.0});
    moveTo(session, Point{40.0, 0.0});
    clickAt(session, Point{40.0, 0.0});

    CHECK(countOf(f.doc, EntityKind::Point) == 2);
    CHECK(countOf(f.doc, EntityKind::Segment) == 1);
    CHECK(f.journal.records().size() == 1);

    // And it undoes back to nothing in one press.
    session.handle(Key::Undo);
    CHECK(f.doc.entities().size() == 0);
}

TEST_CASE("chained segments share their endpoint") {
    // Consecutive segments reuse the point rather than stacking two coincident
    // ones the solver would then have to be told about.
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    session.setTool(ToolKind::Line);

    clickAt(session, Point{-40.0, 0.0});
    clickAt(session, Point{0.0, 0.0});
    clickAt(session, Point{40.0, 30.0});

    CHECK(countOf(f.doc, EntityKind::Point) == 3);
    CHECK(countOf(f.doc, EntityKind::Segment) == 2);

    // The shared point is named by both segments.
    std::vector<EntityId> segments;
    for(const EntityRecord &e : f.doc.entities().records()) {
        if(e.kind == EntityKind::Segment) segments.push_back(e.id);
    }
    REQUIRE(segments.size() == 2);
    const EntityRecord &first = *f.doc.entities().find(segments[0]);
    const EntityRecord &second = *f.doc.entities().find(segments[1]);
    CHECK(first.points[1] == second.points[0]);

    // Each segment is its own undo step, so backing out is incremental.
    CHECK(f.journal.records().size() == 2);
    session.handle(Key::Undo);
    CHECK(countOf(f.doc, EntityKind::Segment) == 1);
}

TEST_CASE("Esc ends the chain, then leaves the tool") {
    // Two presses get home from anywhere, and never more.
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    session.setTool(ToolKind::Line);

    clickAt(session, Point{-40.0, 0.0});
    clickAt(session, Point{0.0, 0.0});
    REQUIRE(session.presentation().toolPreview.active);

    session.handle(Key::Escape);
    CHECK(session.tool() == ToolKind::Line);          // still in the tool
    CHECK_FALSE(session.presentation().toolPreview.active);  // chain ended

    session.handle(Key::Escape);
    CHECK(session.tool() == ToolKind::Select);        // home
}

TEST_CASE("a chain ended by Esc starts a fresh run, not a connected one") {
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    session.setTool(ToolKind::Line);

    clickAt(session, Point{-40.0, 0.0});
    clickAt(session, Point{0.0, 0.0});
    session.handle(Key::Escape);

    clickAt(session, Point{40.0, 40.0});
    clickAt(session, Point{80.0, 40.0});

    // Four points, not three: the second run shares nothing with the first.
    CHECK(countOf(f.doc, EntityKind::Point) == 4);
    CHECK(countOf(f.doc, EntityKind::Segment) == 2);
}

TEST_CASE("the strip reports the placement in flight") {
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    session.setTool(ToolKind::Line);

    // On grid multiples, so this measures the strip rather than the grid snap.
    clickAt(session, Point{0.0, 0.0});
    moveTo(session, Point{60.0, 80.0});

    const std::vector<ToolParameter> &parameters = session.presentation().toolParameters;
    REQUIRE(parameters.size() == 2);
    CHECK(std::string(parameters[0].name) == "length");
    CHECK(parameters[0].value == doctest::Approx(100.0));  // 3-4-5
    CHECK(std::string(parameters[1].name) == "angle");
    // Degrees, measured the way the document is drawn: Y is up.
    CHECK(parameters[1].value == doctest::Approx(53.13010235).epsilon(1e-6));
}

TEST_CASE("the ghost is where commit will put it") {
    // Preview shows truth. Once inference lands this is the property that stops
    // the rubber band and the committed geometry disagreeing.
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    session.setTool(ToolKind::Line);

    clickAt(session, Point{-10.0, -20.0});
    moveTo(session, Point{35.0, 15.0});

    const ToolPreview ghost = session.presentation().toolPreview;
    REQUIRE(ghost.active);

    clickAt(session, Point{35.0, 15.0});
    const Pose pose = session.pose();
    std::vector<EntityId> points;
    for(const EntityRecord &e : f.doc.entities().records()) {
        if(e.kind == EntityKind::Point) points.push_back(e.id);
    }
    REQUIRE(points.size() == 2);
    CHECK(pose.point(points[0])->x == ghost.from.x);
    CHECK(pose.point(points[0])->y == ghost.from.y);
    CHECK(pose.point(points[1])->x == ghost.to.x);
    CHECK(pose.point(points[1])->y == ghost.to.y);
}

TEST_CASE("a creation tool owns the pointer") {
    // Verb-noun: the noun does not exist yet, so there is nothing under the
    // cursor for a click to mean instead. No hit testing, no marquee, no drag.
    Fixture f;
    const EntityId a = paroculus::test::addPoint(f.doc, 0.0, 0.0);
    const EntityId b = paroculus::test::addPoint(f.doc, 60.0, 0.0);
    paroculus::test::addSegment(f.doc, a, b);

    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());

    // In selection, clicking the vertex selects it.
    clickAt(session, Point{0.0, 0.0});
    REQUIRE_FALSE(session.selection().empty());

    // Activating a tool clears it, and clicking the same place now draws.
    session.setTool(ToolKind::Line);
    CHECK(session.selection().empty());
    clickAt(session, Point{0.0, 0.0});
    clickAt(session, Point{0.0, 40.0});
    CHECK(session.selection().empty());
    CHECK(countOf(f.doc, EntityKind::Segment) == 2);
}

TEST_CASE("switching tools abandons what was in flight") {
    Fixture f;
    Session session(f.doc, f.journal);
    session.setViewport(toolViewport());
    session.setTool(ToolKind::Line);

    clickAt(session, Point{0.0, 0.0});
    session.setTool(ToolKind::Select);

    CHECK(session.presentation().toolParameters.empty());
    CHECK_FALSE(session.presentation().toolPreview.active);
    CHECK(f.doc.entities().size() == 0);  // nothing was committed, so nothing leaks
}

TEST_CASE("drawing survives a script round-trip") {
    // The reason the script format was built before the tools: a drawing
    // session is only replayable if the tool changes are part of the input.
    // The same click means "select this" or "place a point here" depending on
    // which tool is in force.
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);

    GestureScript script;
    script.document = doc;
    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(toolViewport());

    session.setTool(ToolKind::Line);
    clickAt(session, Point{-40.0, 0.0});
    clickAt(session, Point{0.0, 0.0});
    clickAt(session, Point{40.0, 30.0});
    session.handle(Key::Escape);
    session.handle(Key::Escape);
    script.steps = recorder.steps();

    const std::string text = serializeScript(script);
    CHECK(text.find("tool name=line") != std::string::npos);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    CHECK(serializeScript(parsed) == text);

    Document replayed = parsed.document;
    UndoJournal journal2;
    Session session2(replayed, journal2);
    replay(session2, parsed);

    CHECK(serialize(replayed) == serialize(doc));
    CHECK(session2.tool() == ToolKind::Select);
}

TEST_CASE("an unknown tool in a script is refused") {
    const char *text =
        "paroculus-script 0\ndocument\nparoculus 0\nend-document\ntool name=nonesuch\n";
    GestureScript out;
    const ScriptLoadResult result = parseScript(text, out);
    CHECK_FALSE(result.ok);
    CHECK(out.steps.empty());
}
