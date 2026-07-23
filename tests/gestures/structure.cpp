// The structure operations, driven as a session and replayed as a script.
//
// Same corpus question stage 6 asked of the composition, for the same reason:
// none of these actions has another recording surface behind it. A rotation is
// not a pointer event, a duplicate is not a keystroke, and a compound relation
// is not a tool change — so each either writes its own step or vanishes from
// every script that used it, silently, with record → replay → record still
// holding because both sides lost it.
//
// It also exercises the pair of questions that make stage 7 what it is. The
// axis answer and the value answer ride as arguments on the actions, so a script
// records which answer the user gave; a flag that failed to round-trip would
// replay the other one, which is the same drawing coming back different.
#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "core/composition.h"
#include "core/persist.h"
#include "core/tags.h"
#include "core/transform.h"
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

// A press carrying a click count, so a test can descend the selection ladder the
// way a double-click does: shape, then edges, then points.
void clickAt(Session &session, Point p, int clicks) {
    const ViewTransform &view = session.viewport().view;
    const Eigen::Vector2d s = view.toScreen(p);
    session.handle(PointerEvent::at(PointerAction::Move, s, view));
    PointerEvent event = PointerEvent::at(PointerAction::Press, s, view, Button::Left);
    event.clicks = clicks;
    session.handle(event);
    session.handle(PointerEvent::at(PointerAction::Release, s, view, Button::Left));
}

// Descends the selection to the points rung at `corner`: one click selects the
// shape, then two double-clicks descend past the edges to the points. Leaving no
// edge selected is what keeps the tag unselected, so a drag from here is an
// ordinary vertex drag rather than the handle drag a bare corner press would be.
void descendToPoints(Session &session, Point corner) {
    clickAt(session, corner, 1);
    clickAt(session, corner, 2);
    clickAt(session, corner, 2);
}

// Drags the pointer from one document point to another, as a user would.
void dragTo(Session &session, Point from, Point to) {
    const ViewTransform &view = session.viewport().view;
    const Eigen::Vector2d a = view.toScreen(from);
    const Eigen::Vector2d b = view.toScreen(to);
    session.handle(PointerEvent::at(PointerAction::Move, a, view));
    session.handle(PointerEvent::at(PointerAction::Press, a, view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Move, b, view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, b, view, Button::Left));
}

// A marquee over a document-space box, which is how a whole shape is named.
void marquee(Session &session, Point from, Point to) {
    const ViewTransform &view = session.viewport().view;
    const Eigen::Vector2d a = view.toScreen(from);
    const Eigen::Vector2d b = view.toScreen(to);
    session.handle(PointerEvent::at(PointerAction::Move, a, view));
    session.handle(PointerEvent::at(PointerAction::Press, a, view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Move, b, view, Button::Left));
    session.handle(PointerEvent::at(PointerAction::Release, b, view, Button::Left));
}

std::unique_ptr<Session> fresh(Document &doc, UndoJournal &journal) {
    auto session = std::make_unique<Session>(doc, journal);
    session->setViewport(benchViewport());
    session->snapPolicy().gridEnabled = false;
    return session;
}

// A rectangle, rotated with the axes retargeted, then duplicated, then the copy
// scaled with its dimensions rescaled. Every selection is a click or a marquee,
// never Session::select, because select is deliberately not a recording surface.
GestureScript structureScript() {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    ScriptRecorder recorder;
    session->setRecorder(&recorder);

    session->setTool(ToolKind::Rectangle);
    press(*session, Point{-80.0, -40.0});
    press(*session, Point{-20.0, 20.0});
    session->setTool(ToolKind::Select);

    marquee(*session, Point{-120.0, -80.0}, Point{20.0, 60.0});

    // The axis question, answered. Retargeting binds the four squaring relations
    // to a cluster frame, so the rectangle turns rigidly instead of the solver
    // pulling it back to the document axes.
    ActionArguments rotate;
    rotate.set("degrees", 20.0);
    rotate.set("retarget", 1.0);
    REQUIRE(invokeAction(*session, "transform.rotate", rotate));

    // Duplicate leaves the copy selected, which is what makes a second
    // duplicate an array rather than a shape on top of a shape.
    ActionArguments offset;
    offset.set("dx", 200.0);
    offset.set("dy", 0.0);
    REQUIRE(invokeAction(*session, "edit.duplicate", offset));

    // And the value question, answered on the copy the duplicate left selected.
    ActionArguments scale;
    scale.set("factor", 2.0);
    scale.set("scale-values", 1.0);
    REQUIRE(invokeAction(*session, "transform.scale", scale));

    GestureScript script;
    script.steps = recorder.steps();
    return script;
}

}  // namespace

TEST_CASE("structure: the gesture reaches the document it describes") {
    const GestureScript script = structureScript();

    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);
    replay(*session, script);

    // Two rectangles: the original and its duplicate, each four edges over eight
    // points, plus the cluster frame the retarget created — a construction
    // segment and its two ends.
    CHECK(doc.tags().size() == 2);
    for(const TagRecord &t : doc.tags().records()) {
        CHECK(t.kind == TagKind::Rectangle);
        CHECK(tagState(doc, t) == TagState::Whole);
    }

    // The frame is there and every axis relation names one.
    size_t frames = 0;
    for(const EntityRecord &e : doc.entities().records()) {
        if(e.kind == EntityKind::Segment && e.role == Role::Construction) frames++;
    }
    CHECK(frames > 0);
    size_t referenced = 0;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind != ConstraintKind::Horizontal && c.kind != ConstraintKind::Vertical) continue;
        if(boundOperandCount(c) == 2) referenced++;
    }
    // Four on the original, and four more on the copy: the copy carried its
    // internal relations, and the frame reference is internal to what was
    // duplicated because the frame was inside the marquee.
    CHECK(referenced == 8);

    // The whole thing solves. A retargeted cluster is under-constrained by the
    // one rotational degree of freedom its frame is now free in, which is what
    // "the subset's horizontal tilts with it" means, not a fault.
    session->refresh();
    CHECK(session->presentation().status == SolveStatus::Okay);
}

TEST_CASE("structure: recording the replay reproduces the script") {
    const GestureScript script = structureScript();
    const std::string text = serializeScript(script);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);

    Document doc = parsed.document;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);
    ScriptRecorder again;
    session->setRecorder(&again);
    replay(*session, parsed);

    GestureScript second;
    second.document = parsed.document;
    second.steps = again.steps();
    CHECK(serializeScript(second) == text);

    // And every recorded action resolves. A step naming something the registry
    // does not have parses, replays as nothing, and re-records as nothing — so
    // the identity above holds while the edit is silently gone.
    size_t actions = 0;
    for(const ScriptStep &step : parsed.steps) {
        if(step.kind != ScriptStep::Kind::Action) continue;
        INFO("action ", step.actionName);
        CHECK(findAction(step.actionName) != nullptr);
        actions++;
    }
    CHECK(actions == 3);
}

TEST_CASE("structure: the answers round-trip, not just the actions") {
    // A flag that failed to round-trip would replay the other answer, and the
    // script would still be byte-identical to itself while describing a
    // different drawing. So the answers are read back out of the text.
    const std::string text = serializeScript(structureScript());
    CHECK(text.find("retarget") != std::string::npos);
    CHECK(text.find("scale-values") != std::string::npos);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    auto argument = [](const ScriptStep &step, std::string_view name) {
        for(const auto &[key, value] : step.arguments) {
            if(key == name) return value;
        }
        return 0.0;
    };
    bool sawRetarget = false;
    bool sawScaleValues = false;
    for(const ScriptStep &step : parsed.steps) {
        if(step.actionName == "transform.rotate") {
            sawRetarget = argument(step, "retarget") != 0.0;
        }
        if(step.actionName == "transform.scale") {
            sawScaleValues = argument(step, "scale-values") != 0.0;
        }
    }
    CHECK(sawRetarget);
    CHECK(sawScaleValues);
}

TEST_CASE("structure: the axis question is previewed before it is answered") {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    session->setTool(ToolKind::Rectangle);
    press(*session, Point{-60.0, -40.0});
    press(*session, Point{60.0, 40.0});
    session->setTool(ToolKind::Select);
    marquee(*session, Point{-100.0, -80.0}, Point{100.0, 80.0});

    const Session::TransformPreview kept =
        session->previewRotate(25.0, AxisAnswer::KeepDocumentAxes);
    const Session::TransformPreview retargeted =
        session->previewRotate(25.0, AxisAnswer::RetargetToClusterFrame);

    // Both answers see the same question, which is what makes it one question.
    REQUIRE(kept.ok());
    REQUIRE(retargeted.ok());
    CHECK(kept.axisConstraints == 4);
    CHECK(retargeted.axisConstraints == 4);

    // And they differ in exactly the way the documentation says: keeping the
    // document axes lets the solver fight the rotation, so the drawing does not
    // stay where the transform put it. Retargeting leaves it alone.
    CHECK(kept.residual > 1.0);
    CHECK(retargeted.residual < 1e-6);

    // The preview changed nothing. The one place a document copy is right, and
    // this is what makes that claim checkable.
    CHECK(doc.tags().size() == 1);
    CHECK(session->presentation().structure.retargeted == 0);
}

TEST_CASE("structure: a rectangle's handle drives its dimensions") {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    session->setTool(ToolKind::Rectangle);
    press(*session, Point{-60.0, -40.0});
    press(*session, Point{60.0, 40.0});
    session->setTool(ToolKind::Select);
    marquee(*session, Point{-100.0, -80.0}, Point{100.0, 80.0});

    REQUIRE(session->selectedTags().size() == 1);
    const TagId tag = session->selectedTags().front();

    // The panel, typed into. An undimensioned side has no slot to drive, so the
    // number becomes the dimension.
    REQUIRE(session->setRectangleWidth(tag, 100.0));
    {
        const std::vector<Session::RectanglePanel> panels = session->rectanglePanels();
        REQUIRE(panels.size() == 1);
        CHECK(panels.front().size.width == doctest::Approx(100.0).epsilon(1e-6));
        CHECK(panels.front().size.widthDimension.valid());
        CHECK(!panels.front().size.heightDimension.valid());
    }

    // Now the handle. Grabbing a corner of a rectangle whose width is driven
    // would ordinarily saturate against that dimension immediately; instead the
    // drag suppresses it and the value follows the hand.
    const std::optional<RectangleFrame> frame = rectangleFrame(doc, tag);
    REQUIRE(frame.has_value());
    const std::optional<std::array<Point, 4>> corners =
        rectangleHandles(session->pose(), *frame);
    REQUIRE(corners.has_value());

    // The corner furthest from the origin, dragged outward along x.
    Point grabbed = (*corners)[0];
    for(const Point &p : *corners) {
        if(p.x > grabbed.x) grabbed = p;
    }
    dragTo(*session, grabbed, Point{grabbed.x + 40.0, grabbed.y});

    const std::vector<Session::RectanglePanel> after = session->rectanglePanels();
    REQUIRE(after.size() == 1);
    // The dimension moved with the handle rather than holding it at 100.
    const ConstraintRecord *width =
        doc.constraints().find(after.front().size.widthDimension);
    REQUIRE(width != nullptr);
    CHECK(width->value.constant() > 110.0);
    CHECK(width->value.constant() == doctest::Approx(after.front().size.width).epsilon(1e-6));
    // Still driving. The handle edited its value; it did not demote it.
    CHECK(width->driving);
}

TEST_CASE("every corner of a whole-selected rectangle drags freely to the cursor") {
    // The stage U4 regression. A single click selects the whole rectangle, which
    // put every corner in the drag's held set; the solver keeps a held param
    // near its seed, and its equal-coordinate substitution (coincidence,
    // horizontal and vertical are all a=b) then eliminated the grab's cursor
    // value in favour of a held neighbour's old seed. The corners came out each
    // locked to a different axis — one vertical only, its neighbour horizontal
    // only — and the corner between them locked on both. Holding only the grab
    // when the whole component is selected lets every corner track the cursor,
    // with the adjacent corners following and the opposite one anchored: the
    // drag-along a corner resize is supposed to be.
    const Point corners[] = {{60.0, 40.0}, {60.0, -40.0}, {-60.0, -40.0}, {-60.0, 40.0}};
    for(const Point &c : corners) {
        Document doc;
        UndoJournal journal;
        std::unique_ptr<Session> session = fresh(doc, journal);
        session->setTool(ToolKind::Rectangle);
        press(*session, Point{-60.0, -40.0});
        press(*session, Point{60.0, 40.0});
        session->setTool(ToolKind::Select);

        // The point ids at the grabbed corner and at the opposite one, read
        // before the drag so they can be followed through it. A corner is two
        // coincident points; both must land on the cursor.
        const Point opposite{-c.x, -c.y};
        std::vector<EntityId> grabbedPts, oppositePts;
        for(const EntityRecord &e : doc.entities().records()) {
            if(e.kind != EntityKind::Point) continue;
            const std::optional<Point> p = session->pose().point(e.id);
            if(!p) continue;
            if(std::hypot(p->x - c.x, p->y - c.y) < 1e-6) grabbedPts.push_back(e.id);
            if(std::hypot(p->x - opposite.x, p->y - opposite.y) < 1e-6) oppositePts.push_back(e.id);
        }
        REQUIRE(grabbedPts.size() == 2);
        REQUIRE(oppositePts.size() == 2);

        const Point target{c.x + 40.0, c.y + 30.0};
        dragTo(*session, c, target);

        // The grabbed corner reached the cursor in full — both coincident points,
        // both coordinates — rather than one axis snapping back to a neighbour.
        for(EntityId id : grabbedPts) {
            const std::optional<Point> p = session->pose().point(id);
            REQUIRE(p.has_value());
            CHECK(p->x == doctest::Approx(target.x).epsilon(1e-6));
            CHECK(p->y == doctest::Approx(target.y).epsilon(1e-6));
        }
        // The opposite corner is the anchor and did not move.
        for(EntityId id : oppositePts) {
            const std::optional<Point> p = session->pose().point(id);
            REQUIRE(p.has_value());
            CHECK(p->x == doctest::Approx(opposite.x).epsilon(1e-6));
            CHECK(p->y == doctest::Approx(opposite.y).epsilon(1e-6));
        }
        CHECK_FALSE(session->presentation().saturated);
    }
}

TEST_CASE("structure: the handle stops working when the tag does, and nothing else does") {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    session->setTool(ToolKind::Rectangle);
    press(*session, Point{-60.0, -40.0});
    press(*session, Point{60.0, 40.0});
    session->setTool(ToolKind::Select);
    marquee(*session, Point{-100.0, -80.0}, Point{100.0, 80.0});
    REQUIRE(session->selectedTags().size() == 1);
    const TagId tag = session->selectedTags().front();
    REQUIRE(session->setRectangleWidth(tag, 90.0));

    const size_t entitiesBefore = doc.entities().size();
    const size_t constraintsBefore = doc.constraints().size();

    // Open one corner. The tag loses a defining relation and stops offering
    // anything; nothing else about the drawing changes.
    ConstraintId join;
    for(const ConstraintRecord &c : doc.constraints().records()) {
        if(c.kind == ConstraintKind::Coincident) {
            join = c.id;
            break;
        }
    }
    REQUIRE(join.valid());
    const ConstraintId doomed[] = {join};
    REQUIRE(journal.applyStep(doc, "open", deletionStep(doc, doomed)) == CommandError::None);
    session->refresh();

    CHECK(!rectangleFrame(doc, tag).has_value());
    CHECK(session->rectanglePanels().empty());
    CHECK(session->presentation().brokenTags.size() == 1);

    // Every primitive survived, the width dimension included: the tag owned
    // none of them, so dissolution costs the affordances and nothing else.
    CHECK(doc.entities().size() == entitiesBefore);
    CHECK(doc.constraints().size() == constraintsBefore - 1);
    CHECK(doc.tags().size() == 1);

    // And the drawing still solves, now with the corner open.
    CHECK(session->presentation().status == SolveStatus::Okay);
}

TEST_CASE("structure: dissolving a tag by hand costs nothing either") {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    session->setTool(ToolKind::Rectangle);
    press(*session, Point{-60.0, -40.0});
    press(*session, Point{60.0, 40.0});
    session->setTool(ToolKind::Select);
    marquee(*session, Point{-100.0, -80.0}, Point{100.0, 80.0});

    const std::string before = serialize(doc);
    REQUIRE(invokeAction(*session, "tag.dissolve"));
    CHECK(doc.tags().size() == 0);

    // One undo puts it back, exactly. The tag is a whole record and its inverse
    // is exact, which is the same property every other degradation relies on.
    session->handle(Key::Undo);
    session->refresh();
    CHECK(serialize(doc) == before);
}

// ---------------------------------------------------------------------------
// Regressions from the stage 7 review
// ---------------------------------------------------------------------------

namespace {

// A tagged rectangle with a driving width, and the session showing it.
struct DimensionedRectangle {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session;
    TagId tag;

    DimensionedRectangle() {
        session = fresh(doc, journal);
        session->setTool(ToolKind::Rectangle);
        press(*session, Point{-60.0, -40.0});
        press(*session, Point{60.0, 40.0});
        session->setTool(ToolKind::Select);
        marquee(*session, Point{-100.0, -80.0}, Point{100.0, 80.0});
        REQUIRE(session->selectedTags().size() == 1);
        tag = session->selectedTags().front();
        REQUIRE(session->setRectangleWidth(tag, 120.0));
    }

    Point corner() const {
        const std::optional<RectangleFrame> frame = rectangleFrame(doc, tag);
        REQUIRE(frame.has_value());
        const std::optional<std::array<Point, 4>> corners =
            rectangleHandles(session->pose(), *frame);
        REQUIRE(corners.has_value());
        Point best = (*corners)[0];
        for(const Point &p : *corners) {
            if(p.x > best.x || (p.x == best.x && p.y > best.y)) best = p;
        }
        return best;
    }

    double widthValue() const {
        const std::optional<RectangleFrame> frame = rectangleFrame(doc, tag);
        const ConstraintId id = edgeDimension(doc, frame->widthEdge);
        const ConstraintRecord *c = doc.constraints().find(id);
        return c != nullptr ? c->value.constant() : -1.0;
    }

    double heightMeasured() const {
        const std::optional<RectangleFrame> frame = rectangleFrame(doc, tag);
        const std::optional<RectangleSize> size =
            rectangleSize(doc, session->pose(), *frame);
        return size ? size->height : -1.0;
    }

    // Every dimension the panel reports, against the length it is on. The
    // invariant a suppressed-and-not-written-back dimension violates: the
    // constraint says one thing and the geometry does another, and the next
    // solve resolves it by dragging the drawing off the number.
    double worstDisagreement() const {
        double worst = 0.0;
        for(const Session::RectanglePanel &panel : session->rectanglePanels()) {
            if(panel.size.widthDimension.valid()) {
                const ConstraintRecord *c =
                    doc.constraints().find(panel.size.widthDimension);
                if(c != nullptr && c->value.isConstant()) {
                    worst = std::max(worst, std::abs(c->value.constant() - panel.size.width));
                }
            }
            if(panel.size.heightDimension.valid()) {
                const ConstraintRecord *c =
                    doc.constraints().find(panel.size.heightDimension);
                if(c != nullptr && c->value.isConstant()) {
                    worst = std::max(worst, std::abs(c->value.constant() - panel.size.height));
                }
            }
        }
        return worst;
    }
};

}  // namespace

TEST_CASE("an ordinary vertex drag does not rewrite a rectangle's dimension") {
    // The handle affordance belongs to the tag, and a tag is named by naming
    // what it is over. Dragging a corner with the tag unselected is an ordinary
    // vertex drag, and a driving width must resist it — otherwise the number the
    // user pinned is silently rewritten by a gesture that never showed a handle.
    //
    // Reaching that gesture takes descending. A bare corner press selects the
    // connected run, edges included, which selects the tag and makes the drag a
    // handle drag — the resize the handle is for. Descending past the edges to
    // the points leaves the tag unselected, which is the ordinary vertex drag
    // this invariant is about.
    DimensionedRectangle f;
    const Point grabbed = f.corner();
    CHECK(f.widthValue() == doctest::Approx(120.0));

    descendToPoints(*f.session, grabbed);
    REQUIRE(f.session->selectedTags().empty());

    const double heightBefore = f.heightMeasured();
    // Diagonally, so the undimensioned side can move even while the dimensioned
    // one resists. That movement is what proves the drag grabbed the corner —
    // without it this test would pass on a drag that grabbed nothing at all.
    dragTo(*f.session, grabbed, Point{grabbed.x + 60.0, grabbed.y + 40.0});
    f.session->refresh();
    CHECK(f.heightMeasured() > heightBefore + 10.0);

    // And the dimension held. The drag saturated against it, which is what a
    // driving dimension is for.
    CHECK(f.widthValue() == doctest::Approx(120.0));
}

TEST_CASE("the numeric twin of a handle drag writes the value back") {
    // A typed value that moved the geometry while its dimension was suppressed
    // has to move the dimension too. Otherwise the constraint returns on the next
    // solve and pulls the drawing straight off the number just confirmed.
    DimensionedRectangle f;
    const Point grabbed = f.corner();

    const ViewTransform &view = f.session->viewport().view;
    const Eigen::Vector2d at = view.toScreen(grabbed);
    f.session->handle(PointerEvent::at(PointerAction::Move, at, view));
    f.session->handle(PointerEvent::at(PointerAction::Press, at, view, Button::Left));
    f.session->handle(PointerEvent::at(PointerAction::Move,
                                       view.toScreen(Point{grabbed.x + 5.0, grabbed.y}), view,
                                       Button::Left));

    // A drag really is in flight, with a field to type into. Without this the
    // test would pass on a press that grabbed nothing.
    REQUIRE(f.session->presentation().dragging);
    REQUIRE(!f.session->presentation().toolParameters.empty());

    for(char c : std::string("200")) f.session->type(c);
    f.session->numericResolve(false);
    f.session->refresh();

    // Whatever the corner drove, the dimension and the geometry agree — so the
    // next solve holds the drawing where the user just confirmed it rather than
    // pulling it back to the value the suppressed constraint still held.
    CHECK(f.worstDisagreement() < 1e-6);
    // The drag is over and nothing is left armed for the next one.
    CHECK_FALSE(f.session->presentation().dragging);
    press(*f.session, Point{-300.0, 250.0});
    const double settled = f.widthValue();
    dragTo(*f.session, Point{-320.0, 260.0}, Point{-310.0, 260.0});
    CHECK(f.widthValue() == doctest::Approx(settled));
}

TEST_CASE("a handle does not sever a dimension driven by a named parameter") {
    DimensionedRectangle f;
    const std::optional<RectangleFrame> frame = rectangleFrame(f.doc, f.tag);
    REQUIRE(frame.has_value());
    const ConstraintId width = edgeDimension(f.doc, frame->widthEdge);
    REQUIRE(width.valid());

    // Drive the width from a named document parameter, as a user sharing one
    // number across several rectangles would.
    ParameterRecord parameter;
    parameter.name = "w";
    parameter.value = Slot(120.0);
    const uint32_t id = f.doc.apply(AddRecord<ParameterRecord>{parameter}).allocated;
    REQUIRE(id != 0);
    ConstraintRecord driven = *f.doc.constraints().find(width);
    driven.value = Slot::parameter(ParameterId(id));
    REQUIRE(f.doc.apply(SetRecord<ConstraintRecord>{driven}).ok());
    f.session->refresh();

    const Point grabbed = f.corner();
    marquee(*f.session, Point{-200.0, -160.0}, Point{200.0, 160.0});
    REQUIRE(f.session->selectedTags().size() == 1);
    const double heightBefore = f.heightMeasured();
    dragTo(*f.session, grabbed, Point{grabbed.x + 60.0, grabbed.y + 40.0});
    f.session->refresh();
    // The drag landed on the corner: the undimensioned side moved.
    CHECK(f.heightMeasured() > heightBefore + 10.0);

    // The slot is still the parameter reference. A bare constant here would sever
    // this rectangle from every other one sharing `w`, silently: editing `w`
    // afterwards would move all of them and leave this one behind.
    const ConstraintRecord *after = f.doc.constraints().find(width);
    REQUIRE(after != nullptr);
    CHECK_FALSE(after->value.isConstant());
    CHECK(after->value.references().size() == 1);
}

TEST_CASE("a transform turns about the seeds it rewrites") {
    // The centre has to be a point in seed space, because the rewrite is in seed
    // space. Taking it from a solved pose that differs from the seeds rotates the
    // cluster about a foreign point, so the shape is translated as well as turned
    // and jumps instead of spinning in place.
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 40.0, 0.0);
    const EntityId c = addPoint(doc, 40.0, 40.0);
    const EntityId first = addSegment(doc, a, b);
    const EntityId second = addSegment(doc, b, c);
    // A relation the seeds do not satisfy, so the solved pose is somewhere the
    // seeds are not — which is the ordinary case, since seeds are never written
    // back from a solve.
    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b}, Slot(90.0));
    session->refresh();
    session->select({a, b, c, first, second});

    const std::optional<Point> centre = session->transformCentre();
    REQUIRE(centre.has_value());
    // The seed cluster spans x in [0, 40] and y in [0, 40], so its centre is
    // (20, 20) whatever the solver has done with it on screen.
    CHECK(centre->x == doctest::Approx(20.0));
    CHECK(centre->y == doctest::Approx(20.0));

    // A half turn about that centre maps the seed cluster onto itself reflected
    // through it, so the seed bounding box is unchanged. A centre taken from the
    // pose would translate it.
    REQUIRE(session->rotateSelection(180.0, AxisAnswer::KeepDocumentAxes));
    double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
    for(EntityId id : {a, b, c}) {
        const EntityRecord *e = doc.entities().find(id);
        REQUIRE(e != nullptr);
        minX = std::min(minX, e->seeds[0]);
        maxX = std::max(maxX, e->seeds[0]);
        minY = std::min(minY, e->seeds[1]);
        maxY = std::max(maxY, e->seeds[1]);
    }
    CHECK(minX == doctest::Approx(0.0));
    CHECK(maxX == doctest::Approx(40.0));
    CHECK(minY == doctest::Approx(0.0));
    CHECK(maxY == doctest::Approx(40.0));
}

TEST_CASE("the panel downgrades a width it cannot drive") {
    // Every other imposition path checks before committing. A rectangle whose
    // width edge is already determined would otherwise take an unchecked driving
    // constraint, go inconsistent, and freeze at its committed seeds with no
    // verdict and no downgrade shown.
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    session->setTool(ToolKind::Rectangle);
    press(*session, Point{-60.0, -40.0});
    press(*session, Point{60.0, 40.0});
    session->setTool(ToolKind::Select);
    marquee(*session, Point{-100.0, -80.0}, Point{100.0, 80.0});
    const TagId tag = session->selectedTags().front();

    const std::optional<RectangleFrame> frame = rectangleFrame(doc, tag);
    REQUIRE(frame.has_value());
    const EntityRecord *edge = doc.entities().find(frame->widthEdge);
    REQUIRE(edge != nullptr);
    // Pin both ends of the width edge: its length is now determined by geometry
    // that cannot move, so no driving width can hold.
    addConstraint(doc, ConstraintKind::Pin, {edge->points[0]});
    addConstraint(doc, ConstraintKind::Pin, {edge->points[1]});
    session->refresh();

    session->setRectangleWidth(tag, 500.0);
    session->refresh();

    // Whatever it decided, it did not leave a driving constraint the system
    // cannot satisfy: the document still solves, which is the property the check
    // exists to preserve.
    CHECK(session->presentation().status != SolveStatus::Inconsistent);
}

TEST_CASE("mirror reports the relations it dropped at the boundary") {
    // mirrorStep copies through copyStep, which sheds the null-reference axis
    // relations a reflection cannot preserve. Those drops are the no-silent-changes
    // half of the operation, so the session's structure report has to carry the
    // count out — a mirror that discarded it would quietly hand back geometry less
    // constrained than what it reflected.
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);

    // An L squared against the document frame: two null-reference axis relations,
    // the meaning a tilted reflection cannot keep.
    const EntityId a = addPoint(doc, 10.0, 0.0);
    const EntityId b = addPoint(doc, 60.0, 0.0);
    const EntityId c = addPoint(doc, 60.0, 30.0);
    const EntityId across = addSegment(doc, a, b);
    const EntityId up = addSegment(doc, b, c);
    addConstraint(doc, ConstraintKind::Horizontal, {across});
    addConstraint(doc, ConstraintKind::Vertical, {up});

    // A tilted construction guide, which the selection tells apart from the bars.
    const EntityId axisFrom = addPoint(doc, -40.0, -40.0);
    const EntityId axisTo = addPoint(doc, 40.0, 40.0);
    const EntityId axis = addSegment(doc, axisFrom, axisTo);
    EntityRecord guide = *doc.entities().find(axis);
    guide.role = Role::Construction;
    REQUIRE(doc.apply(SetRecord<EntityRecord>{guide}).ok());
    session->refresh();

    session->select({across, up, axis});
    REQUIRE(invokeAction(*session, "relation.mirror"));

    // Both axis relations straddled the world frame the reflection tilts away
    // from, so both were dropped — and the count reached the surface. Today the
    // report reads 0 because mirrorStep discarded what the copy counted.
    CHECK(session->presentation().structure.droppedRelations == 2);
}
