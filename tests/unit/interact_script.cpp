#include <doctest/doctest.h>

#include "core/persist.h"
#include "interact/registry.h"
#include "interact/script.h"
#include "interact/session.h"
#include "solve/demosketch.h"
#include "support/build.h"

using namespace paroculus;

namespace {

Viewport testViewport(double scale = 1.0) {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(scale, -scale));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

// A recorded session over the demo: hover, drag an endpoint, undo. Enough
// shapes that the format has to carry every step kind.
struct Recording {
    Document start;   // the document as recording began
    Document ended;   // where the session left it
    GestureScript script;
};

Recording record() {
    Document doc = demoDocument(1.618);
    UndoJournal journal;
    Session session(doc, journal);

    Recording out;
    out.start = doc;  // after settle: a load is not an edit, so this is the baseline

    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(testViewport());

    const EntityId a1 = doc.entities().records()[1].id;
    const Viewport &v = session.viewport();
    const Eigen::Vector2d from = v.view.toScreen(*session.pose().point(a1));

    session.handle(PointerEvent::at(PointerAction::Move, from, v.view));
    session.handle(PointerEvent::at(PointerAction::Press, from, v.view, Button::Left));
    for(int i = 1; i <= 6; i++) {
        session.handle(PointerEvent::at(PointerAction::Move,
                                        from + Eigen::Vector2d(0.0, -7.0 * i), v.view,
                                        Button::Left, Modifier::Shift));
    }
    session.handle(PointerEvent::at(PointerAction::Release, from + Eigen::Vector2d(0.0, -42.0),
                                    v.view, Button::Left));
    session.handle(Key::Escape);

    out.script.document = out.start;
    out.script.steps = recorder.steps();
    out.ended = doc;
    return out;
}

}  // namespace

TEST_CASE("a script round-trips byte-identically") {
    const Recording r = record();
    const std::string text = serializeScript(r.script);

    GestureScript parsed;
    const ScriptLoadResult loaded = parseScript(text, parsed);
    REQUIRE(loaded.ok);
    CHECK(serializeScript(parsed) == text);
    CHECK(parsed.steps.size() == r.script.steps.size());
    CHECK(serialize(parsed.document) == serialize(r.script.document));
}

TEST_CASE("a recording captures every kind of step") {
    const Recording r = record();
    bool viewport = false, pointer = false, key = false, modifiers = false;
    for(const ScriptStep &s : r.script.steps) {
        viewport = viewport || s.kind == ScriptStep::Kind::Viewport;
        pointer = pointer || s.kind == ScriptStep::Kind::Pointer;
        key = key || s.kind == ScriptStep::Kind::Key;
        modifiers = modifiers || has(s.modifiers, Modifier::Shift);
    }
    CHECK(viewport);
    CHECK(pointer);
    CHECK(key);
    CHECK(modifiers);
}

TEST_CASE("replaying a script reproduces the document it recorded") {
    // The property the whole artefact rests on. If replay landed anywhere but
    // where the original session landed, a script would be a story about a
    // session rather than the session itself, and neither the corpus nor a
    // person watching playback would be looking at what actually happened.
    const Recording r = record();

    GestureScript parsed;
    REQUIRE(parseScript(serializeScript(r.script), parsed).ok);

    Document doc = parsed.document;
    UndoJournal journal;
    Session session(doc, journal);
    replay(session, parsed);

    CHECK(serialize(doc) == serialize(r.ended));
}

TEST_CASE("re-recording a replay reproduces the script") {
    // Record → replay → record must be the identity. This is what makes a
    // script editable by hand with confidence: the file is the whole input.
    const Recording r = record();
    const std::string text = serializeScript(r.script);

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
}

TEST_CASE("opening a document does not modify it") {
    // Replay builds a Session over the script's embedded document. If opening
    // one were an edit, every replay would begin from a different place than
    // the recording did — and worse, opening a file and closing it would have
    // changed it.
    //
    // This is not hypothetical. A Newton solve is not a fixpoint in the last
    // bits: solving the solved document oscillates between two answers about
    // 1.4e-14 apart, so a settle-on-open that committed its result would give a
    // different file on every open, forever.
    Document doc = demoDocument(1.618);
    const std::string before = serialize(doc);

    UndoJournal journal;
    Session session(doc, journal);
    CHECK(serialize(doc) == before);

    // Opening it repeatedly is equally inert.
    for(int i = 0; i < 4; i++) {
        UndoJournal again;
        Session reopened(doc, again);
        CHECK(serialize(doc) == before);
    }

    // And what is shown is nonetheless the solved pose, not the raw seeds:
    // derived, cached, never stored.
    Document fresh = demoDocument(1.618);
    UndoJournal j;
    Session shown(fresh, j);
    const EntityId a1 = fresh.entities().records()[1].id;
    CHECK(fresh.entities().records()[1].seeds[1] == 18.0);   // seed, off-constraint
    CHECK(shown.pose().point(a1)->y == doctest::Approx(0.0));  // solved, horizontal
}

TEST_CASE("a script survives a viewport change mid-session") {
    // Screen coordinates only mean something relative to the transform in
    // force, so a pan or zoom has to land in the file as a step.
    Document doc = demoDocument(1.618);
    UndoJournal journal;
    Session session(doc, journal);

    GestureScript script;
    script.document = doc;
    ScriptRecorder recorder;
    session.setRecorder(&recorder);

    const EntityId a1 = doc.entities().records()[1].id;
    session.setViewport(testViewport(1.0));
    const Eigen::Vector2d at1x =
        session.viewport().view.toScreen(*session.pose().point(a1));
    session.handle(PointerEvent::at(PointerAction::Move, at1x, session.viewport().view));

    session.setViewport(testViewport(3.0));
    const Eigen::Vector2d at3x =
        session.viewport().view.toScreen(*session.pose().point(a1));
    session.handle(PointerEvent::at(PointerAction::Move, at3x, session.viewport().view));
    REQUIRE(session.presentation().hovered == a1);

    script.steps = recorder.steps();

    GestureScript parsed;
    REQUIRE(parseScript(serializeScript(script), parsed).ok);
    Document replayed = parsed.document;
    UndoJournal journal2;
    Session session2(replayed, journal2);
    replay(session2, parsed);
    // Under the wrong transform the second position would miss the vertex.
    CHECK(session2.presentation().hovered == a1);
}

TEST_CASE("exact coordinates survive the format") {
    // Screen positions are doubles and a script is the storage of record for
    // them, so display-friendly rounding has no business here.
    Document doc;
    GestureScript script;
    script.document = doc;

    ScriptStep viewport;
    viewport.kind = ScriptStep::Kind::Viewport;
    viewport.viewport = testViewport();
    script.steps.push_back(viewport);

    ScriptStep step;
    step.kind = ScriptStep::Kind::Pointer;
    step.action = PointerAction::Move;
    step.screen = Eigen::Vector2d(1.0 / 3.0, 0.1 + 0.2);
    script.steps.push_back(step);

    GestureScript parsed;
    REQUIRE(parseScript(serializeScript(script), parsed).ok);
    REQUIRE(parsed.steps.size() == 2);
    CHECK(parsed.steps[1].screen.x() == step.screen.x());
    CHECK(parsed.steps[1].screen.y() == step.screen.y());
    // And the view transform, which every screen coordinate is read through.
    CHECK(parsed.steps[0].viewport.view.matrix().matrix() ==
          viewport.viewport.view.matrix().matrix());
}

TEST_CASE("malformed scripts are refused rather than half-read") {
    struct Case { const char *what; const char *text; };
    const Case cases[] = {
        {"empty", ""},
        {"not a script", "something else 0\n"},
        {"newer version", "paroculus-script 99\ndocument\nparoculus 0\nend-document\n"},
        {"no document", "paroculus-script 0\nmove screen=1,1\n"},
        {"unterminated document", "paroculus-script 0\ndocument\nparoculus 0\n"},
        {"unknown step", "paroculus-script 0\ndocument\nparoculus 0\nend-document\nwiggle x=1\n"},
        {"unknown key", "paroculus-script 0\ndocument\nparoculus 0\nend-document\nkey name=nope\n"},
        {"pointer without position",
         "paroculus-script 0\ndocument\nparoculus 0\nend-document\nmove button=left\n"},
        {"singular viewport", "paroculus-script 0\ndocument\nparoculus 0\nend-document\n"
                              "viewport width=8 height=6 m=0,0,0,0,0,0\n"},
    };
    for(const Case &c : cases) {
        GestureScript out;
        // Seed it, so a parser that fails without clearing is caught.
        out.steps.push_back(ScriptStep{});
        const ScriptLoadResult result = parseScript(c.text, out);
        INFO(c.what);
        CHECK_FALSE(result.ok);
        CHECK_FALSE(result.error.empty());
        CHECK(out.steps.empty());
    }
}

TEST_CASE("comments and blank lines are ignored") {
    const char *text =
        "paroculus-script 0\n"
        "\n"
        "# what this script demonstrates\n"
        "document\n"
        "paroculus 0\n"
        "end-document\n"
        "\n"
        "# the gesture itself\n"
        "viewport width=800 height=600 m=1,0,0,-1,400,300\n"
        "press screen=10,20 button=left\n"
        "release screen=10,20 button=left\n";
    GestureScript parsed;
    const ScriptLoadResult result = parseScript(text, parsed);
    REQUIRE(result.ok);
    CHECK(parsed.steps.size() == 3);
}

TEST_CASE("a hand-written script drives a session") {
    // The format is meant to be written by hand during feel iteration, not only
    // produced by the recorder.
    Document doc = demoDocument(1.618);
    UndoJournal journal;
    Session probe(doc, journal);
    probe.setViewport(testViewport());
    const EntityId a1 = doc.entities().records()[1].id;
    const Eigen::Vector2d target = probe.viewport().view.toScreen(*probe.pose().point(a1));

    std::string text = "paroculus-script 0\ndocument\n";
    text += serialize(doc);
    text += "end-document\n";
    text += "viewport width=800 height=600 m=1,0,0,-1,400,300\n";
    text += "move screen=" + std::to_string(target.x()) + "," + std::to_string(target.y()) + "\n";

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);

    Document replayed = parsed.document;
    UndoJournal journal2;
    Session session(replayed, journal2);
    replay(session, parsed);
    CHECK(session.presentation().hovered == a1);
}

namespace {

// A recorded authoring session: a tool, an offer confirmed, and a placement
// finished by typing rather than by clicking. The drag recording above exercises
// pointer and key steps; these are the step kinds a stage 4 session produces and
// that one never reaches.
Recording recordAuthoring() {
    Document doc;
    // A reference at 30 degrees, clear of both axes, so what a run alongside it
    // generates is an offer and not something that commits on its own.
    const EntityId a = paroculus::test::addPoint(doc, -100.0, -100.0);
    const EntityId b = paroculus::test::addPoint(doc, 100.0, -42.0);
    paroculus::test::addSegment(doc, a, b);

    UndoJournal journal;
    Session session(doc, journal);

    Recording out;
    out.start = doc;

    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    session.setViewport(testViewport());
    session.snapPolicy().gridEnabled = false;

    const Viewport &v = session.viewport();
    auto moveTo = [&](Point p) {
        session.handle(PointerEvent::at(PointerAction::Move, v.view.toScreen(p), v.view));
    };
    auto pressAt = [&](Point p) {
        moveTo(p);
        session.handle(
            PointerEvent::at(PointerAction::Press, v.view.toScreen(p), v.view, Button::Left));
    };

    invokeAction(session, "tool.line");
    pressAt(Point{-100.0, 40.0});
    moveTo(Point{100.0, 100.0});

    // Confirm the offer, then finish the segment by typing a length rather than
    // clicking it — every gesture has a numeric twin, and Enter finishes it.
    ScriptStep confirm;
    confirm.kind = ScriptStep::Kind::Confirm;
    confirm.index = 0;
    applyStep(session, confirm);

    session.type('1');
    session.type('5');
    session.type('0');
    session.numericAdvance();
    session.numericBackspace();
    session.numericResolve(false);

    session.handle(Key::Escape);
    session.handle(Key::Escape);

    out.script.document = out.start;
    out.script.steps = recorder.steps();
    out.ended = doc;
    return out;
}

}  // namespace

TEST_CASE("an authoring recording captures the step kinds a drag never reaches") {
    const Recording r = recordAuthoring();
    bool tool = false, confirm = false, type = false, resolve = false;
    bool backspace = false, advance = false;
    for(const ScriptStep &s : r.script.steps) {
        tool = tool || s.kind == ScriptStep::Kind::Tool;
        confirm = confirm || s.kind == ScriptStep::Kind::Confirm;
        type = type || s.kind == ScriptStep::Kind::Type;
        resolve = resolve || s.kind == ScriptStep::Kind::NumericResolve;
        backspace = backspace || s.kind == ScriptStep::Kind::NumericBackspace;
        advance = advance || s.kind == ScriptStep::Kind::NumericAdvance;
    }
    // The tool arrived through invokeAction and was recorded as the state
    // change it made, not as the action that made it. See the Action-step test
    // below for what that costs a hand-written script.
    CHECK(tool);
    CHECK(confirm);
    CHECK(type);
    CHECK(resolve);
    CHECK(backspace);
    CHECK(advance);
}

TEST_CASE("replaying an authoring script reproduces the document it recorded") {
    const Recording r = recordAuthoring();

    GestureScript parsed;
    REQUIRE(parseScript(serializeScript(r.script), parsed).ok);

    Document doc = parsed.document;
    UndoJournal journal;
    Session session(doc, journal);
    replay(session, parsed);

    CHECK(serialize(doc) == serialize(r.ended));
}

TEST_CASE("re-recording an authoring replay reproduces the script") {
    // Record → replay → record over the numeric and confirm steps too. These
    // dispatch through paths a drag never touches — a recording surface that
    // wrote a step and then called a public method that wrote it again would
    // double every one of them, and only a re-record notices.
    const Recording r = recordAuthoring();
    const std::string text = serializeScript(r.script);

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
}

TEST_CASE("a hand-written action step survives replay as what it did") {
    // Action steps are parseable and replayable but nothing records one: the
    // recording surfaces sit at the keystroke, and invokeAction is a core they
    // dispatch to rather than a surface of its own. So a hand-written action
    // replays correctly and re-records as the state change it caused.
    //
    // Record → replay → record is still the identity over anything a session
    // produced. It is not byte-identity over a hand-written action, and this is
    // where that shows: the file is still the whole input, it just comes back
    // spelled as the effect rather than the cause.
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(testViewport());

    GestureScript script;
    script.document = doc;
    ScriptStep action;
    action.kind = ScriptStep::Kind::Action;
    action.actionName = "tool.circle";
    script.steps.push_back(action);

    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    replay(session, script);

    // The action did what it says.
    CHECK(session.tool() == ToolKind::Circle);

    REQUIRE(recorder.steps().size() == 1);
    CHECK(recorder.steps()[0].kind == ScriptStep::Kind::Tool);
    CHECK(recorder.steps()[0].tool == ToolKind::Circle);

    // And the re-recorded script replays to the same place, which is the
    // property that actually matters: a script is a recording of a session, and
    // both spellings are recordings of this one.
    Document second = script.document;
    UndoJournal secondJournal;
    Session secondSession(second, secondJournal);
    secondSession.setViewport(testViewport());
    GestureScript rerecorded;
    rerecorded.document = script.document;
    rerecorded.steps = recorder.steps();
    replay(secondSession, rerecorded);
    CHECK(secondSession.tool() == ToolKind::Circle);
}
