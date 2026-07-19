#include <doctest/doctest.h>

#include <cmath>

#include "core/persist.h"
#include "interact/script.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;

namespace {

Viewport entryViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

struct Entry {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;

    explicit Entry(ToolKind tool) {
        session = std::make_unique<Session>(doc, journal);
        session->setViewport(entryViewport());
        session->snapPolicy().gridEnabled = false;
        session->setTool(tool);
    }

    void moveTo(Point p) {
        session->handle(PointerEvent::at(PointerAction::Move,
                                         session->viewport().view.toScreen(p),
                                         session->viewport().view));
    }
    void click(Point p) {
        moveTo(p);
        session->handle(PointerEvent::at(PointerAction::Press,
                                         session->viewport().view.toScreen(p),
                                         session->viewport().view, Button::Left));
    }
    void typeText(const char *text) {
        for(const char *c = text; *c != 0; c++) session->type(*c);
    }
    double valueOf(ConstraintKind kind) const {
        for(const ConstraintRecord &c : doc.constraints().records()) {
            if(c.kind == kind) return c.value.constant();
        }
        return -1.0;
    }
    bool has(ConstraintKind kind) const {
        for(const ConstraintRecord &c : doc.constraints().records()) {
            if(c.kind == kind) return true;
        }
        return false;
    }
};

}  // namespace

TEST_CASE("typing a length resolves the placement exactly") {
    // The whole reason this entrance exists is that a drag cannot land on a
    // number. Landing near it would defeat the point.
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{37.0, 0.0});  // roughly, by hand

    e.typeText("45");
    CHECK(e.session->presentation().numericActive);
    CHECK(e.session->presentation().numericText == "45");

    // Enter finishes the gesture. Asking for a click afterwards would ask the
    // hand for the precision the digits were supplying — and the click would
    // arrive at the pointer, which is still sitting at the 37 the hand managed.
    e.session->handle(Key::Enter);
    CHECK_FALSE(e.session->presentation().numericActive);

    const Pose pose = e.session->pose();
    std::vector<EntityId> points;
    for(const EntityRecord &r : e.doc.entities().records()) {
        if(r.kind == EntityKind::Point) points.push_back(r.id);
    }
    REQUIRE(points.size() == 2);
    const Point a = *pose.point(points[0]);
    const Point b = *pose.point(points[1]);
    CHECK(std::hypot(b.x - a.x, b.y - a.y) == doctest::Approx(45.0).epsilon(1e-12));
}

TEST_CASE("resolving alone declares nothing; imposing declares a dimension") {
    // Approximate gesture and exact entry are two entrances to the same edit.
    // Making the value hold is not the same as declaring that it must.
    SUBCASE("resolve") {
        Entry e(ToolKind::Line);
        e.click(Point{0.0, 0.0});
        e.moveTo(Point{37.0, 0.0});
        e.typeText("45");
        e.session->handle(Key::Enter);
        CHECK_FALSE(e.has(ConstraintKind::PointPointDistance));
    }
    SUBCASE("impose") {
        Entry e(ToolKind::Line);
        e.click(Point{0.0, 0.0});
        e.moveTo(Point{37.0, 0.0});
        e.typeText("45");
        // One more key.
        e.session->handle(Key::Enter, Modifier::Shift);

        REQUIRE(e.has(ConstraintKind::PointPointDistance));
        // The stored value is exactly what was typed, not a re-parse of what
        // was displayed: display rounding never round-trips into storage.
        CHECK(e.valueOf(ConstraintKind::PointPointDistance) == 45.0);
        // And the dimension rides the same undo step as the geometry.
        CHECK(e.journal.records().size() == 1);
        e.session->handle(Key::Undo);
        CHECK(e.doc.constraints().size() == 0);
        CHECK(e.doc.entities().size() == 0);
    }
}

TEST_CASE("units convert at the one boundary") {
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{10.0, 0.0});
    e.typeText("2cm");
    e.session->handle(Key::Enter, Modifier::Shift);

    // Storage is millimetres, always. Nothing above units.h holds centimetres.
    CHECK(e.valueOf(ConstraintKind::PointPointDistance) == doctest::Approx(20.0));
}

TEST_CASE("a typed angle steers the placement but declares no dimension") {
    // An angle constraint is an angle *to* something, and this gesture has
    // drawn only one segment. Resolving is honest; imposing would need a
    // reference the user never named.
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{50.0, 5.0});

    // Tab opens the first field, then cycles: length, then angle.
    e.session->handle(Key::Tab);
    CHECK(e.session->presentation().numericTarget == 0);
    e.session->handle(Key::Tab);
    CHECK(e.session->presentation().numericTarget == 1);
    e.typeText("90");
    e.session->handle(Key::Enter, Modifier::Shift);

    // Straight up from the anchor, at the length the hand had chosen.
    const Pose pose = e.session->pose();
    std::vector<Point> placed;
    for(const EntityRecord &r : e.doc.entities().records()) {
        if(r.kind == EntityKind::Point) placed.push_back(*pose.point(r.id));
    }
    REQUIRE(placed.size() == 2);
    CHECK(placed[1].x == doctest::Approx(0.0));
    CHECK(placed[1].y > 0.0);
    CHECK_FALSE(e.has(ConstraintKind::Angle));
}

TEST_CASE("a typed radius pins a circle") {
    Entry e(ToolKind::Circle);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{13.0, 0.0});
    e.typeText("30");
    e.session->handle(Key::Enter, Modifier::Shift);

    REQUIRE(e.has(ConstraintKind::Radius));
    CHECK(e.valueOf(ConstraintKind::Radius) == 30.0);

    for(const EntityRecord &r : e.doc.entities().records()) {
        if(r.kind != EntityKind::Circle) continue;
        CHECK(e.session->pose().radius(r.id) == doctest::Approx(30.0));
    }
}

TEST_CASE("typed width and height pin a rectangle") {
    Entry e(ToolKind::Rectangle);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{55.0, 33.0});

    e.typeText("80");
    e.session->handle(Key::Enter, Modifier::Shift);

    REQUIRE(e.has(ConstraintKind::PointPointDistance));
    CHECK(e.valueOf(ConstraintKind::PointPointDistance) == 80.0);
    // Four axis relations plus four coincidences plus the dimension, over
    // sixteen parameters: one degree of freedom fewer than an undimensioned
    // rectangle, so position and height remain.
    CHECK(e.session->presentation().dof == 3);
}

TEST_CASE("typing sets magnitude and leaves direction to the hand") {
    // Typing a width must not flip a rectangle the user drew to the left.
    Entry e(ToolKind::Rectangle);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{-55.0, -33.0});
    e.typeText("80");
    e.session->handle(Key::Enter);

    const Pose pose = e.session->pose();
    double minX = 0.0, minY = 0.0;
    for(const EntityRecord &r : e.doc.entities().records()) {
        if(r.kind != EntityKind::Point) continue;
        const Point p = *pose.point(r.id);
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
    }
    CHECK(minX == doctest::Approx(-80.0));
    CHECK(minY < 0.0);
}

TEST_CASE("Esc takes back the field before the placement") {
    // Esc ascends one level at a time.
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{40.0, 0.0});
    e.typeText("45");
    REQUIRE(e.session->presentation().numericActive);

    e.session->handle(Key::Escape);
    CHECK_FALSE(e.session->presentation().numericActive);
    CHECK(e.session->presentation().toolPreview.active);  // the placement survives

    e.session->handle(Key::Escape);
    CHECK_FALSE(e.session->presentation().toolPreview.active);
    CHECK(e.session->tool() == ToolKind::Line);

    e.session->handle(Key::Escape);
    CHECK(e.session->tool() == ToolKind::Select);
}

TEST_CASE("backspace edits the field") {
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{40.0, 0.0});
    e.typeText("456");
    e.session->numericBackspace();
    CHECK(e.session->presentation().numericText == "45");
    e.session->handle(Key::Enter);

    const Pose pose = e.session->pose();
    std::vector<Point> placed;
    for(const EntityRecord &r : e.doc.entities().records()) {
        if(r.kind == EntityKind::Point) placed.push_back(*pose.point(r.id));
    }
    REQUIRE(placed.size() == 2);
    CHECK(std::hypot(placed[1].x - placed[0].x, placed[1].y - placed[0].y) ==
          doctest::Approx(45.0));
}

TEST_CASE("an incomplete or impossible value resolves nothing") {
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{40.0, 0.0});

    e.typeText("12mmm");  // trailing garbage is a parse failure, not a truncation
    e.session->handle(Key::Enter);
    CHECK(e.session->presentation().toolPreview.to.x == doctest::Approx(40.0));

    e.session->numericCancel();
    e.typeText("0");  // a zero-length segment is not a segment
    e.session->handle(Key::Enter);
    CHECK(e.session->presentation().toolPreview.to.x == doctest::Approx(40.0));
}

TEST_CASE("display rounding never reaches storage") {
    // formatLength is lossy by design, so a value that renders short must still
    // be stored whole.
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{40.0, 0.0});
    e.typeText("45.123456789");
    e.session->handle(Key::Enter, Modifier::Shift);

    const double stored = e.valueOf(ConstraintKind::PointPointDistance);
    CHECK(stored == 45.123456789);
    // What a strip would show is not what is kept.
    CHECK(formatLength(stored, Unit::Millimetre, 2) == "45.12mm");
    CHECK(stored != 45.12);
}

TEST_CASE("numeric entry survives a script round-trip") {
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    GestureScript script;
    script.document = doc;
    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(entryViewport());
    session.setTool(ToolKind::Line);

    auto click = [&](Point p) {
        const Eigen::Vector2d s = session.viewport().view.toScreen(p);
        session.handle(PointerEvent::at(PointerAction::Move, s, session.viewport().view));
        session.handle(
            PointerEvent::at(PointerAction::Press, s, session.viewport().view, Button::Left));
    };
    click(Point{0.0, 0.0});
    session.handle(PointerEvent::at(PointerAction::Move,
                                    session.viewport().view.toScreen(Point{40.0, 0.0}),
                                    session.viewport().view));
    for(char c : std::string("60")) session.type(c);
    session.handle(Key::Enter, Modifier::Shift);
    script.steps = recorder.steps();

    const std::string text = serializeScript(script);
    CHECK(text.find("type char=6") != std::string::npos);
    // The keystroke, once. A surface records what it was asked to do; the work
    // it dispatches to must not record a second step for the same event.
    CHECK(text.find("key name=enter mods=shift") != std::string::npos);
    CHECK(text.find("numeric do=impose") == std::string::npos);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    CHECK(serializeScript(parsed) == text);

    Document replayed = parsed.document;
    UndoJournal journal2;
    Session session2(replayed, journal2);
    replay(session2, parsed);
    CHECK(serialize(replayed) == serialize(doc));
}

TEST_CASE("record → replay → record is the identity over keys that drive numeric entry") {
    // The property the format exists to guarantee, over the steps that most
    // easily break it. handle(Key) records the keystroke and then does the work
    // itself; if the work also recorded, a live Tab would replay as two Tabs,
    // the replayed session would diverge from the recorded one, and every
    // re-recording would grow another step per key.
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(entryViewport());
    session.setTool(ToolKind::Line);

    auto at = [&](Point p) { return session.viewport().view.toScreen(p); };
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{0.0, 0.0}),
                                    session.viewport().view));
    session.handle(PointerEvent::at(PointerAction::Press, at(Point{0.0, 0.0}),
                                    session.viewport().view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{40.0, 5.0}),
                                    session.viewport().view));

    // Every key that dispatches into numeric entry: Tab cycles the field, Esc
    // takes it back, Enter finishes.
    session.handle(Key::Tab);
    const size_t firstField = session.presentation().numericTarget;
    session.handle(Key::Tab);
    CHECK(session.presentation().numericTarget != firstField);
    session.handle(Key::Escape);
    CHECK_FALSE(session.presentation().numericActive);
    for(char c : std::string("55")) session.type(c);
    session.handle(Key::Enter);

    GestureScript first;
    first.document = Document();
    first.steps = recorder.steps();
    const std::string firstText = serializeScript(first);

    // Replay it under a recorder of its own, then compare the recordings.
    Document replayedDoc = first.document;
    UndoJournal replayedJournal;
    Session replayedSession(replayedDoc, replayedJournal);
    ScriptRecorder second;
    replayedSession.setRecorder(&second);
    replay(replayedSession, first);

    GestureScript again;
    again.document = first.document;
    again.steps = second.steps();
    CHECK(serializeScript(again) == firstText);
    // And the replay reached the same document, not one that advanced twice.
    CHECK(serialize(replayedDoc) == serialize(doc));
}

TEST_CASE("a typed space survives the round trip") {
    // The length grammar accepts a space — "45 mm" is a value a user types —
    // and fields on a script line are space-delimited, so an unescaped one
    // would split the line and the parser would refuse a file the recorder had
    // just written.
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(entryViewport());
    session.setTool(ToolKind::Line);

    auto at = [&](Point p) { return session.viewport().view.toScreen(p); };
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{0.0, 0.0}),
                                    session.viewport().view));
    session.handle(PointerEvent::at(PointerAction::Press, at(Point{0.0, 0.0}),
                                    session.viewport().view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{40.0, 0.0}),
                                    session.viewport().view));
    for(char c : std::string("45 mm")) session.type(c);
    CHECK(session.presentation().numericText == "45 mm");
    session.handle(Key::Enter, Modifier::Shift);

    GestureScript script;
    script.document = Document();
    script.steps = recorder.steps();
    const std::string text = serializeScript(script);

    GestureScript parsed;
    REQUIRE_MESSAGE(parseScript(text, parsed).ok, text);
    CHECK(serializeScript(parsed) == text);
    // Round-tripped as the character it was, not as whatever the escape reads
    // like: the replay has to type the same key the user pressed.
    bool sawSpace = false;
    for(const ScriptStep &step : parsed.steps) {
        if(step.kind == ScriptStep::Kind::Type && step.character == ' ') sawSpace = true;
    }
    CHECK(sawSpace);
}
