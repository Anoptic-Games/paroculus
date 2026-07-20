#include <doctest/doctest.h>

#include <cmath>

#include "core/persist.h"
#include "interact/script.h"
#include "interact/registry.h"
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

TEST_CASE("a typed radius pins an arc") {
    // Stage 4's goal line is that every gesture gains its numeric twin. The arc
    // was the one that had not: its radius and sweep were display-only, so
    // typing during an arc gesture did nothing at all.
    Entry e(ToolKind::Arc);
    e.click(Point{-50.0, 0.0});
    e.click(Point{50.0, 0.0});
    e.moveTo(Point{0.0, 20.0});  // a shallow bulge, by hand

    e.typeText("80");
    e.session->handle(Key::Enter, Modifier::Shift);

    REQUIRE(e.has(ConstraintKind::Radius));
    CHECK(e.valueOf(ConstraintKind::Radius) == 80.0);

    const Pose pose = e.session->pose();
    for(const EntityRecord &r : e.doc.entities().records()) {
        if(r.kind != EntityKind::Arc) continue;
        const std::optional<Pose::ArcGeometry> g = pose.arc(r.id);
        REQUIRE(g.has_value());
        CHECK(g->radius == doctest::Approx(80.0));
    }
}

TEST_CASE("a typed sweep resolves through the chord") {
    // Sweep and radius are the same construction seen from two sides: the chord
    // is fixed once two clicks have landed, so c = 2r sin(t/2) turns one into
    // the other.
    Entry e(ToolKind::Arc);
    e.click(Point{-50.0, 0.0});
    e.click(Point{50.0, 0.0});
    e.moveTo(Point{0.0, 20.0});

    // Tab to the sweep field, then a half turn: the chord becomes a diameter.
    e.session->handle(Key::Tab);
    e.session->handle(Key::Tab);
    CHECK(e.session->presentation().numericTarget == 1);
    e.typeText("180");
    e.session->handle(Key::Enter);

    const Pose pose = e.session->pose();
    bool sawArc = false;
    for(const EntityRecord &r : e.doc.entities().records()) {
        if(r.kind != EntityKind::Arc) continue;
        sawArc = true;
        const std::optional<Pose::ArcGeometry> g = pose.arc(r.id);
        REQUIRE(g.has_value());
        // A semicircle on a chord of 100.
        CHECK(g->radius == doctest::Approx(50.0));
        CHECK(g->centre.x == doctest::Approx(0.0));
        CHECK(g->centre.y == doctest::Approx(0.0));
    }
    CHECK(sawArc);
}

TEST_CASE("typing an arc keeps the bulge on the side the hand chose") {
    // The same rule the rectangle's width follows: magnitude from the digits,
    // sign from the hand. Typing a radius must not flip an arc the user has
    // already curved one way.
    for(double bulge : {30.0, -30.0}) {
        CAPTURE(bulge);
        Entry e(ToolKind::Arc);
        e.click(Point{-50.0, 0.0});
        e.click(Point{50.0, 0.0});
        e.moveTo(Point{0.0, bulge});
        e.typeText("80");
        e.session->handle(Key::Enter);

        const Pose pose = e.session->pose();
        bool sawArc = false;
        for(const EntityRecord &r : e.doc.entities().records()) {
            if(r.kind != EntityKind::Arc) continue;
            sawArc = true;
            const std::optional<Pose::ArcGeometry> g = pose.arc(r.id);
            REQUIRE(g.has_value());
            CHECK(g->radius == doctest::Approx(80.0));
            // The centre sits opposite the bulge.
            CHECK(g->centre.y * bulge < 0.0);
        }
        // Guarded, or a tool that resolved nothing would pass this vacuously —
        // there would simply be no arc to disagree with.
        REQUIRE(sawArc);
    }
}

TEST_CASE("an arc refuses a radius no chord of that length admits") {
    // No arc through a chord of length c has a radius below c/2. Asking for one
    // is a value the gesture cannot hold, and resolving nothing is the honest
    // answer — the placement stays where the hand left it.
    Entry e(ToolKind::Arc);
    e.click(Point{-50.0, 0.0});
    e.click(Point{50.0, 0.0});
    e.moveTo(Point{0.0, 20.0});

    e.typeText("10");  // the chord is 100, so the smallest arc radius is 50
    e.session->handle(Key::Enter);
    // Nothing committed: an unresolvable value leaves the gesture in flight.
    CHECK(e.doc.entities().records().empty());
    CHECK(e.session->presentation().toolPreview.active);
}

TEST_CASE("an imposed dimension is checked before it is committed") {
    // Over-constraint's first moment: PRINCIPLES has the candidate solved
    // speculatively before commit, and redundancy flagged at creation because
    // it is where later edits go to die — two constraints that agree today
    // disagree after the next value edit, and the user who added the second one
    // was told nothing.
    Document doc;
    // Two points pinned where they stand, so the distance between them is
    // already fully determined by what is declared.
    const EntityId a = paroculus::test::addPoint(doc, 0.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 100.0, 0.0);
    paroculus::test::addConstraint(doc, ConstraintKind::Pin, {a});
    paroculus::test::addConstraint(doc, ConstraintKind::Pin, {b});

    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(entryViewport());
    session.snapPolicy().gridEnabled = false;
    session.setTool(ToolKind::Line);

    auto at = [&](Point p) { return session.viewport().view.toScreen(p); };
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{0.0, 0.0}),
                                    session.viewport().view));
    session.handle(PointerEvent::at(PointerAction::Press, at(Point{0.0, 0.0}),
                                    session.viewport().view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{60.0, 0.0}),
                                    session.viewport().view));

    // A length that lands the far end exactly on the other pinned point, so
    // both endpoints inherit a coincidence and the dimension restates what the
    // pins already fix.
    for(char c : std::string("100")) session.type(c);
    session.handle(Key::Enter, Modifier::Shift);

    const ConstraintRecord *dimension = nullptr;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind == ConstraintKind::PointPointDistance) dimension = &c;
    }
    REQUIRE(dimension != nullptr);
    CHECK(session.presentation().impositionVerdict == CandidateVerdict::Redundant);
    // Flagged, not refused. Redundant-but-consistent is tolerated at solve time
    // and the solver says so itself, which is the state the flag exists to warn
    // about rather than to prevent.
    CHECK(dimension->driving);
    CHECK(session.presentation().status == SolveStatus::RedundantOkay);
    CHECK(session.presentation().conflicting.empty());

    // And the geometry is exactly where both pins say, so the redundancy is a
    // warning about the next value edit rather than a fault today.
    const Pose pose = session.pose();
    CHECK(pose.point(a)->x == doctest::Approx(0.0));
    CHECK(pose.point(b)->x == doctest::Approx(100.0));
}

TEST_CASE("a dimension that holds is committed driving, and says so") {
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{37.0, 0.0});
    e.typeText("45");
    e.session->handle(Key::Enter, Modifier::Shift);

    REQUIRE(e.has(ConstraintKind::PointPointDistance));
    for(const ConstraintRecord &c : e.doc.constraints().records()) {
        if(c.kind == ConstraintKind::PointPointDistance) CHECK(c.driving);
    }
    CHECK(e.session->presentation().impositionVerdict == CandidateVerdict::Consistent);
    CHECK(e.session->presentation().conflicting.empty());
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

TEST_CASE("a fresh placement inherits no dimension from an abandoned one") {
    // Type a value, impose it, abandon the chain, draw something else: the new
    // geometry must not pick up the old dimension. This used to be reachable
    // because the imposition waited for a commit click and nothing cleared it
    // in between; Enter commits now, so an imposition never outlives the
    // placement that asked for it. Held as a test rather than as a comment,
    // because the failure was silent — a length nobody typed on a shape nobody
    // measured.
    Entry e(ToolKind::Line);
    e.click(Point{0.0, 0.0});
    e.moveTo(Point{37.0, 0.0});
    e.typeText("45");
    e.session->handle(Key::Enter, Modifier::Shift);
    REQUIRE(e.has(ConstraintKind::PointPointDistance));
    CHECK(e.valueOf(ConstraintKind::PointPointDistance) == 45.0);

    size_t dimensions = 0;
    for(const ConstraintRecord &c : e.doc.constraints().records()) {
        if(c.kind == ConstraintKind::PointPointDistance) dimensions++;
    }
    REQUIRE(dimensions == 1);

    // Esc twice: end the chain, then leave the tool. Then draw a fresh shape.
    e.session->handle(Key::Escape);
    e.session->handle(Key::Escape);
    e.session->setTool(ToolKind::Rectangle);
    e.click(Point{200.0, 200.0});
    e.click(Point{260.0, 240.0});

    // Still exactly the one dimension, on the segment it was typed for.
    dimensions = 0;
    for(const ConstraintRecord &c : e.doc.constraints().records()) {
        if(c.kind == ConstraintKind::PointPointDistance) dimensions++;
    }
    CHECK(dimensions == 1);
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

// ---------------------------------------------------------------------------
// The numeric twin of a drag
// ---------------------------------------------------------------------------
//
// Stage 4 built the twin a creation tool has: place, type, enter. Typing during
// a drag of geometry that already exists waited for this stage, because "the
// length under adjustment" is ambiguous the moment a vertex belongs to more
// than one segment and prose cannot resolve it — the strip has to.

namespace {

// A vertex shared by two segments of different lengths, so "the length under
// adjustment" is a real question rather than a rhetorical one.
struct Corner {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;
    EntityId shared, alongX, alongY, first, second;

    Corner() {
        shared = paroculus::test::addPoint(doc, 0.0, 0.0);
        alongX = paroculus::test::addPoint(doc, 60.0, 0.0);
        alongY = paroculus::test::addPoint(doc, 0.0, 40.0);
        first = paroculus::test::addSegment(doc, shared, alongX);
        second = paroculus::test::addSegment(doc, shared, alongY);
        session = std::make_unique<Session>(doc, journal);
        session->setViewport(entryViewport());
    }

    void grab(EntityId id) {
        const ViewTransform &view = session->viewport().view;
        const Eigen::Vector2d at = view.toScreen(*session->pose().point(id));
        session->select({id});
        session->handle(PointerEvent::at(PointerAction::Press, at, view, Button::Left));
        // Far enough to pass the drag threshold, so a drag is genuinely running.
        session->handle(PointerEvent::at(PointerAction::Move,
                                         at + Eigen::Vector2d(25.0, 0.0), view, Button::Left));
    }
    void type(const char *text) {
        for(const char *c = text; *c != 0; c++) session->type(*c);
    }
};

}  // namespace

TEST_CASE("a drag offers every measurement it is adjusting") {
    Corner c;
    c.grab(c.alongX);

    // One dimension per segment the grabbed point ends. Which one the user
    // meant is the question, and the strip is where it is asked.
    REQUIRE(c.session->presentation().dragging);
    const std::vector<ToolParameter> &fields = c.session->presentation().toolParameters;
    REQUIRE(fields.size() == 1);  // alongX ends only the first segment

    // The shared corner ends both, so dragging it offers both.
    c.session->handle(PointerEvent::at(PointerAction::Release,
                                       c.session->viewport().view.toScreen(
                                           *c.session->pose().point(c.alongX)),
                                       c.session->viewport().view, Button::Left));
    Corner d;
    d.grab(d.shared);
    CHECK(d.session->presentation().toolParameters.size() == 2);
}

TEST_CASE("typing during a drag lands the geometry on the number exactly") {
    Corner c;
    c.grab(c.alongX);
    c.type("100");
    REQUIRE(c.session->presentation().numericActive);

    c.session->numericResolve(false);

    // Exactly, not nearly: the reason the entrance exists is that a drag cannot
    // land on a number.
    const Point a = *c.session->pose().point(c.shared);
    const Point b = *c.session->pose().point(c.alongX);
    CHECK(std::hypot(b.x - a.x, b.y - a.y) == doctest::Approx(100.0).epsilon(1e-9));

    // Enter finished the gesture, and nothing was recorded — a resolved drag
    // moves geometry, it does not declare anything.
    CHECK_FALSE(c.session->presentation().dragging);
    CHECK(c.doc.constraints().records().empty());
    CHECK(c.session->canUndo());
}

TEST_CASE("tab picks which length the digits are about") {
    Corner c;
    c.grab(c.shared);
    REQUIRE(c.session->presentation().toolParameters.size() == 2);

    // Second field, so the digits land on the other segment's length.
    c.session->numericAdvance();
    c.session->numericAdvance();
    REQUIRE(c.session->presentation().numericTarget == 1);
    c.type("25");
    c.session->numericResolve(false);

    const Point shared = *c.session->pose().point(c.shared);
    const Point x = *c.session->pose().point(c.alongX);
    const Point y = *c.session->pose().point(c.alongY);
    const double toX = std::hypot(x.x - shared.x, x.y - shared.y);
    const double toY = std::hypot(y.x - shared.x, y.y - shared.y);
    // Whichever field index 1 named got the value; the point is that one of
    // them is exactly 25 and the machinery chose deterministically.
    CHECK(std::min(std::fabs(toX - 25.0), std::fabs(toY - 25.0)) < 1e-9);
}

TEST_CASE("shift+enter during a drag imposes the value as a dimension") {
    Corner c;
    c.grab(c.alongX);
    c.type("75");
    c.session->numericResolve(true);

    // The geometry landed on it and the value was pinned, in one undo step —
    // which is why no imposition outlives the gesture that asked for it.
    REQUIRE(c.doc.constraints().records().size() == 1);
    const ConstraintRecord &r = c.doc.constraints().records().front();
    CHECK(r.kind == ConstraintKind::PointPointDistance);
    CHECK(r.driving);
    CHECK(c.doc.evaluate(r.value).value_or(0.0) == doctest::Approx(75.0));

    REQUIRE(invokeAction(*c.session, "edit.undo"));
    CHECK(c.doc.constraints().records().empty());
}

TEST_CASE("a value the constraints cannot reach leaves the pose alone") {
    // A drag that cannot reach a number is saturation, not a licence to move
    // somewhere else. SolveSpace leaves parameters at the seeds it was handed,
    // so an unguarded read of a refused solve looks like a perfect landing.
    Corner c;
    paroculus::test::addConstraint(c.doc, ConstraintKind::PointPointDistance,
                                   {c.shared, c.alongX}, Slot(60.0));
    c.session->refresh();
    c.grab(c.alongX);

    const Point before = *c.session->pose().point(c.alongX);
    c.type("500");
    c.session->numericResolve(false);

    const Point after = *c.session->pose().point(c.alongX);
    CHECK(std::hypot(after.x - before.x, after.y - before.y) < 1e-9);
    // Still dragging, field still open: the user can try another number.
    CHECK(c.session->presentation().dragging);
}

TEST_CASE("digits open a field during a drag, though the tool is Select") {
    // Keyboard resolution is the registry's, not the shell's — so this is
    // reachable headlessly, which is exactly how the digits once came to be
    // unable to open a field while the documentation said they could.
    Corner c;
    c.grab(c.alongX);

    ActionContext context = contextOf(*c.session);
    REQUIRE(context.tool == ToolKind::Select);
    REQUIRE(context.dragging);

    KeyStroke stroke;
    stroke.character = '4';
    stroke.digit = 4;
    const KeyBinding binding = resolveKey(context, stroke);
    CHECK(binding.kind == KeyBinding::Kind::Text);
    CHECK(binding.character == '4');
}

// ---------------------------------------------------------------------------
// Grabbing an arc endpoint has a numeric twin too
// ---------------------------------------------------------------------------

namespace {

// An arc drawn through the tool, so its centre, endpoints and through-point are
// exactly what a user's gesture makes. Draws a shallow arc on a chord of 100.
struct DrawnArc {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;
    EntityId arc, start, end, centre;

    DrawnArc() {
        session = std::make_unique<Session>(doc, journal);
        session->setViewport(entryViewport());
        session->snapPolicy().gridEnabled = false;
        session->setTool(ToolKind::Arc);
        click(Point{-50.0, 0.0});  // start
        click(Point{50.0, 0.0});   // end
        click(Point{0.0, 30.0});   // through, fixing the bulge
        session->setTool(ToolKind::Select);
        for(const EntityRecord &e : doc.entities().records()) {
            if(e.kind == EntityKind::Arc) {
                arc = e.id;
                centre = e.points[0];
                start = e.points[1];
                end = e.points[2];
            }
        }
    }

    void click(Point p) {
        const ViewTransform &view = session->viewport().view;
        const Eigen::Vector2d s = view.toScreen(p);
        session->handle(PointerEvent::at(PointerAction::Move, s, view));
        session->handle(PointerEvent::at(PointerAction::Press, s, view, Button::Left));
    }

    void grab(EntityId id) {
        const ViewTransform &view = session->viewport().view;
        const Eigen::Vector2d at = view.toScreen(*session->pose().point(id));
        session->select({id});
        session->handle(PointerEvent::at(PointerAction::Press, at, view, Button::Left));
        session->handle(PointerEvent::at(PointerAction::Move, at + Eigen::Vector2d(10.0, 6.0),
                                         view, Button::Left));
    }
    void release() {
        session->handle(PointerEvent::at(PointerAction::Release,
                                         session->viewport().view.toScreen(Point{0.0, 0.0}),
                                         session->viewport().view, Button::Left));
    }
    void type(const char *text) {
        for(const char *c = text; *c != 0; c++) session->type(*c);
    }
    std::optional<Pose::ArcGeometry> geometry() const { return session->pose().arc(arc); }
    double chord() const {
        const Pose pose = session->pose();
        const Point a = *pose.point(start);
        const Point b = *pose.point(end);
        return std::hypot(b.x - a.x, b.y - a.y);
    }
};

}  // namespace

TEST_CASE("grabbing an arc endpoint offers its radius and its chord") {
    DrawnArc a;
    REQUIRE(a.arc.valid());
    a.grab(a.start);

    // The blind spot curves-as-boundaries left: dragDimensions collected segment
    // lengths only, so grabbing an arc's endpoint offered no field at all, and
    // the arc alone among gestures had no numeric twin for its resize.
    REQUIRE(a.session->presentation().dragging);
    const std::vector<ToolParameter> &fields = a.session->presentation().toolParameters;
    REQUIRE(fields.size() == 2);
    // Radius first — the value an endpoint drag usually reaches for — then the
    // chord, and each is named for what it sets rather than by its index.
    CHECK(std::string(fields[0].name) == "radius");
    CHECK(std::string(fields[1].name) == "chord");
    // The values are what the arc currently reads.
    CHECK(fields[0].value == doctest::Approx(a.geometry()->radius));
    CHECK(fields[1].value == doctest::Approx(a.chord()));
}

TEST_CASE("typing a radius resizes the arc exactly") {
    DrawnArc a;
    a.grab(a.start);
    // Field 0 is the radius.
    a.type("60");
    REQUIRE(a.session->presentation().numericActive);
    REQUIRE(a.session->presentation().numericTarget == 0);
    a.session->numericResolve(false);
    a.release();
    a.session->refresh();

    CHECK(a.geometry()->radius == doctest::Approx(60.0).epsilon(1e-6));
    // A resolved drag moves geometry and declares nothing: no radius constraint
    // was created, unlike the shift+enter path below.
    bool declared = false;
    for(const ConstraintRecord &r : a.doc.constraints().records()) {
        if(r.kind == ConstraintKind::Radius) declared = true;
    }
    CHECK_FALSE(declared);
    CHECK_FALSE(a.session->presentation().dragging);
}

TEST_CASE("tab reaches the chord and typing sets it exactly") {
    DrawnArc a;
    a.grab(a.start);
    // Tab opens field 0, Tab again advances to the chord.
    a.session->numericAdvance();
    a.session->numericAdvance();
    REQUIRE(a.session->presentation().numericTarget == 1);
    a.type("140");
    a.session->numericResolve(false);
    a.release();
    a.session->refresh();

    // A drag cannot land on a number; the twin does, exactly.
    CHECK(a.chord() == doctest::Approx(140.0).epsilon(1e-6));
}

TEST_CASE("shift+enter on an arc endpoint imposes the radius as a dimension") {
    DrawnArc a;
    a.grab(a.start);
    a.type("70");
    a.session->numericResolve(true);
    a.session->refresh();

    // The geometry landed on it and the value was pinned, in one undo step.
    bool sawRadius = false;
    for(const ConstraintRecord &r : a.doc.constraints().records()) {
        if(r.kind != ConstraintKind::Radius) continue;
        sawRadius = true;
        CHECK(r.driving);
        CHECK(a.doc.evaluate(r.value).value_or(0.0) == doctest::Approx(70.0));
    }
    CHECK(sawRadius);
    CHECK(a.geometry()->radius == doctest::Approx(70.0).epsilon(1e-6));

    REQUIRE(invokeAction(*a.session, "edit.undo"));
    bool stillHas = false;
    for(const ConstraintRecord &r : a.doc.constraints().records()) {
        if(r.kind == ConstraintKind::Radius) stillHas = true;
    }
    CHECK_FALSE(stillHas);
}

TEST_CASE("an arc endpoint resize survives a script round-trip") {
    // Every gesture the session produces is record → replay → record identity,
    // and a drag's numeric steps are ordinary recording surfaces. The arc twin
    // adds a Radius dimension to a drag, so it has to round-trip like the rest.
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(entryViewport());
    session.snapPolicy().gridEnabled = false;

    ScriptRecorder recorder;
    session.setRecorder(&recorder);

    session.setTool(ToolKind::Arc);
    auto click = [&](Point p) {
        const ViewTransform &view = session.viewport().view;
        const Eigen::Vector2d s = view.toScreen(p);
        session.handle(PointerEvent::at(PointerAction::Move, s, view));
        session.handle(PointerEvent::at(PointerAction::Press, s, view, Button::Left));
    };
    click(Point{-50.0, 0.0});
    click(Point{50.0, 0.0});
    click(Point{0.0, 30.0});
    session.setTool(ToolKind::Select);

    EntityId start;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind == EntityKind::Arc) start = e.points[1];
    }
    REQUIRE(start.valid());
    const ViewTransform &view = session.viewport().view;
    const Eigen::Vector2d at = view.toScreen(*session.pose().point(start));
    session.select({start});
    session.handle(PointerEvent::at(PointerAction::Press, at, view, Button::Left));
    session.handle(
        PointerEvent::at(PointerAction::Move, at + Eigen::Vector2d(10.0, 6.0), view, Button::Left));
    for(char c : std::string("70")) session.type(c);
    session.numericResolve(true);  // impose, so a constraint lands to compare
    session.handle(PointerEvent::at(PointerAction::Release, at, view, Button::Left));

    GestureScript script;
    script.document = Document();
    script.steps = recorder.steps();
    const std::string text = serializeScript(script);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);

    Document replayDoc;
    UndoJournal replayJournal;
    Session replaySession(replayDoc, replayJournal);
    replaySession.setViewport(entryViewport());
    replaySession.snapPolicy().gridEnabled = false;
    ScriptRecorder again;
    replaySession.setRecorder(&again);
    replay(replaySession, parsed);

    GestureScript second;
    second.document = Document();
    second.steps = again.steps();
    CHECK(serializeScript(second) == text);

    // And the replay reproduced the resize: a driving radius of 70.
    bool sawRadius = false;
    for(const ConstraintRecord &r : replayDoc.constraints().records()) {
        if(r.kind != ConstraintKind::Radius) continue;
        sawRadius = true;
        CHECK(replayDoc.evaluate(r.value).value_or(0.0) == doctest::Approx(70.0));
    }
    CHECK(sawRadius);
}
