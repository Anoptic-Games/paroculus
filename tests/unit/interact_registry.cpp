#include <doctest/doctest.h>

#include <set>

#include "core/persist.h"
#include "interact/registry.h"
#include "interact/script.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;

namespace {

Viewport registryViewport() {
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
        session->setViewport(registryViewport());
        session->snapPolicy().gridEnabled = false;
    }
    void click(Point p) {
        const Eigen::Vector2d s = session->viewport().view.toScreen(p);
        session->handle(PointerEvent::at(PointerAction::Move, s, session->viewport().view));
        session->handle(
            PointerEvent::at(PointerAction::Press, s, session->viewport().view, Button::Left));
    }
};

}  // namespace

TEST_CASE("the catalogue is well formed") {
    // A table is only a single source of truth if nothing in it is malformed:
    // a nameless action is unreachable, and a duplicate name makes reachability
    // ambiguous.
    std::set<std::string_view> names;
    std::set<std::string_view> bindings;
    for(const Action &a : actions()) {
        CHECK_FALSE(a.name.empty());
        CHECK_FALSE(a.title.empty());
        CHECK(a.applicable != nullptr);
        CHECK(a.invoke != nullptr);

        INFO("action ", a.name);
        CHECK(names.insert(a.name).second);
        if(!a.binding.empty()) CHECK(bindings.insert(a.binding).second);

        for(const ActionParameter &p : a.parameters) CHECK_FALSE(p.name.empty());

        // Reachable by the name it publishes.
        CHECK(findAction(a.name) == &a);
    }
    CHECK_FALSE(actions().empty());
}

TEST_CASE("every action is invocable headlessly") {
    // The conformance sweep. The registry is the automation surface as well as
    // the UI's, so an action that cannot be driven without a window is an
    // action the corpus can never exercise.
    for(const Action &a : actions()) {
        Bench b;
        // Enough state that most predicates pass: something drawn, something
        // selected, something to undo.
        b.session->setTool(ToolKind::Line);
        b.click(Point{-40.0, 0.0});
        b.click(Point{40.0, 0.0});
        b.session->setTool(ToolKind::Select);
        b.click(Point{40.0, 0.0});

        ActionArguments arguments;
        for(const ActionParameter &p : a.parameters) arguments.set(p.name, 0.0);

        INFO("action ", a.name);
        const ActionContext context = contextOf(*b.session);
        const bool applies = a.applicable(context, a);
        const bool ran = invokeAction(*b.session, a.name, arguments);
        // Applicable actions run; inapplicable ones refuse. Either way the call
        // is answered rather than crashing or silently doing something else.
        CHECK(ran == applies);
    }
}

TEST_CASE("an unknown action is refused") {
    Bench b;
    CHECK_FALSE(invokeAction(*b.session, "tool.nonesuch"));
    CHECK(findAction("tool.nonesuch") == nullptr);
}

TEST_CASE("applicability is checked before the action runs") {
    // An action inapplicable in the model is offerable by no surface, and
    // invoking it anyway must change nothing.
    Bench b;
    const std::string before = serialize(b.doc);

    // Nothing selected, so delete does not apply.
    REQUIRE(b.session->signature().empty());
    CHECK_FALSE(invokeAction(*b.session, "edit.delete"));
    CHECK(serialize(b.doc) == before);

    // Nothing journalled, so undo does not apply.
    CHECK_FALSE(invokeAction(*b.session, "edit.undo"));
    CHECK(serialize(b.doc) == before);
}

TEST_CASE("the schema is enforced once, for every caller") {
    // A missing required parameter fails the same way whoever asked, so no
    // action has to remember to check.
    Bench b;
    b.session->setTool(ToolKind::Line);
    b.click(Point{-40.0, 0.0});

    const Action *confirm = findAction("inference.confirm");
    REQUIRE(confirm != nullptr);
    REQUIRE(confirm->parameters.size() == 1);
    CHECK(confirm->parameters[0].required);

    // No index at all.
    CHECK_FALSE(invokeAction(*b.session, "inference.confirm"));

    // An index that is not a whole number is a caller error, not a rounding
    // opportunity.
    ActionArguments fractional;
    fractional.set("index", 1.5);
    CHECK_FALSE(invokeAction(*b.session, "inference.confirm", fractional));
}

TEST_CASE("actions do what the session does") {
    // The registry is a projection, not a reimplementation: driving through it
    // has to land in the same place as driving the session directly.
    Bench direct;
    direct.session->setTool(ToolKind::Line);
    direct.click(Point{-40.0, 0.0});
    direct.click(Point{40.0, 0.0});
    direct.session->handle(Key::Undo);

    Bench viaRegistry;
    REQUIRE(invokeAction(*viaRegistry.session, "tool.line"));
    viaRegistry.click(Point{-40.0, 0.0});
    viaRegistry.click(Point{40.0, 0.0});
    REQUIRE(invokeAction(*viaRegistry.session, "edit.undo"));

    CHECK(serialize(viaRegistry.doc) == serialize(direct.doc));
    CHECK(viaRegistry.session->tool() == direct.session->tool());
}

TEST_CASE("every tool is reachable through the registry") {
    // Keyboard reachability falls out of the table rather than being maintained
    // beside it, so a tool with no action is a tool no surface can offer.
    const ToolKind kinds[] = {ToolKind::Select, ToolKind::Line, ToolKind::Circle,
                              ToolKind::Arc, ToolKind::Rectangle};
    for(ToolKind kind : kinds) {
        Bench b;
        const std::string name = std::string("tool.") + toolName(kind);
        INFO(name);
        REQUIRE(findAction(name) != nullptr);
        REQUIRE(invokeAction(*b.session, name));
        CHECK(b.session->tool() == kind);
    }
}

TEST_CASE("a script drives the registry") {
    // The scripting harness is a projection of the registry rather than a
    // parallel path into the session.
    Document doc;
    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(registryViewport());
    session.snapPolicy().gridEnabled = false;

    GestureScript script;
    script.document = doc;
    ScriptRecorder recorder;
    session.setRecorder(&recorder);
    recorder.action("tool.rectangle", {});
    replay(session, script);  // no steps yet; the recorder holds them

    script.steps = recorder.steps();
    const std::string text = serializeScript(script);
    CHECK(text.find("action name=tool.rectangle") != std::string::npos);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    CHECK(serializeScript(parsed) == text);

    Document replayed = parsed.document;
    UndoJournal journal2;
    Session session2(replayed, journal2);
    session2.setViewport(registryViewport());
    replay(session2, parsed);
    CHECK(session2.tool() == ToolKind::Rectangle);
}

TEST_CASE("a script naming an action this build lacks is refused") {
    const char *text =
        "paroculus-script 0\ndocument\nparoculus 0\nend-document\naction name=tool.nonesuch\n";
    GestureScript out;
    const ScriptLoadResult result = parseScript(text, out);
    CHECK_FALSE(result.ok);
    CHECK(out.steps.empty());
}

TEST_CASE("action arguments round-trip through a script") {
    GestureScript script;
    ScriptStep step;
    step.kind = ScriptStep::Kind::Action;
    step.actionName = "inference.confirm";
    step.arguments = {{"index", 2.0}};
    script.steps.push_back(step);

    const std::string text = serializeScript(script);
    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    REQUIRE(parsed.steps.size() == 1);
    CHECK(parsed.steps[0].actionName == "inference.confirm");
    REQUIRE(parsed.steps[0].arguments.size() == 1);
    CHECK(parsed.steps[0].arguments[0].first == "index");
    CHECK(parsed.steps[0].arguments[0].second == 2.0);
}

// ---------------------------------------------------------------------------
// Keyboard resolution
// ---------------------------------------------------------------------------

namespace {

KeyStroke digitKey(int digit, Modifier modifiers = Modifier::None) {
    KeyStroke k;
    k.digit = digit;
    // What an unshifted digit key prints. A shifted one prints whatever the
    // layout says, which is exactly why the rank is carried separately.
    if(modifiers == Modifier::None) k.character = static_cast<char>('0' + digit);
    k.modifiers = modifiers;
    return k;
}

KeyStroke charKey(char c, Modifier modifiers = Modifier::None) {
    KeyStroke k;
    k.character = c;
    k.modifiers = modifiers;
    return k;
}

}  // namespace

TEST_CASE("a bare digit types, even with offers on screen") {
    // The collision this policy exists to settle. Numeric entry and offer
    // confirmation are live in the same state, so a digit cannot mean one thing
    // when an offer happens to be visible and another when it is not — that is
    // a keystroke whose meaning the user cannot predict, and it is how typing
    // "45" used to confirm offer four and drop the five.
    ActionContext context;
    context.tool = ToolKind::Line;
    context.offers = 3;

    for(int digit = 1; digit <= 9; digit++) {
        CAPTURE(digit);
        const KeyBinding binding = resolveKey(context, digitKey(digit));
        CHECK(binding.kind == KeyBinding::Kind::Text);
        CHECK(binding.character == static_cast<char>('0' + digit));
    }
    // And the keys a value can also start with.
    for(char c : {'0', '.', '-'}) {
        CHECK(resolveKey(context, charKey(c)).kind == KeyBinding::Kind::Text);
    }
}

TEST_CASE("alt confirms an offer and shift declines an inference") {
    ActionContext context;
    context.tool = ToolKind::Line;
    context.offers = 4;
    context.inferred = 4;

    const KeyBinding confirm = resolveKey(context, digitKey(3, Modifier::Alt));
    REQUIRE(confirm.kind == KeyBinding::Kind::Action);
    CHECK(confirm.action->name == "inference.confirm");
    // Rank three is index two: the strip counts from one, the model from zero.
    CHECK(confirm.arguments.value("index") == 2.0);

    const KeyBinding decline = resolveKey(context, digitKey(3, Modifier::Shift));
    REQUIRE(decline.kind == KeyBinding::Kind::Action);
    CHECK(decline.action->name == "inference.decline");
    CHECK(decline.arguments.value("index") == 2.0);
}

TEST_CASE("a shifted digit is read off the key, not off what it printed") {
    // shift+4 prints a dollar sign on one layout and something else on the
    // next. The offer at rank four is neither.
    ActionContext context;
    context.tool = ToolKind::Line;
    context.inferred = 9;

    KeyStroke stroke;
    stroke.digit = 4;
    stroke.character = '$';
    stroke.modifiers = Modifier::Shift;

    const KeyBinding binding = resolveKey(context, stroke);
    REQUIRE(binding.kind == KeyBinding::Kind::Action);
    CHECK(binding.action->name == "inference.decline");
    CHECK(binding.arguments.value("index") == 3.0);
}

TEST_CASE("an open field swallows the letters a tool is bound to") {
    // Units are spelled with the same letters as the tools, so "45mm" must not
    // activate the rectangle tool halfway through the value.
    ActionContext open;
    open.tool = ToolKind::Line;
    open.numericActive = true;
    for(char c : {'m', 'c', 'r', 'a', 'l', 'v'}) {
        CAPTURE(c);
        const KeyBinding binding = resolveKey(open, charKey(c));
        CHECK(binding.kind == KeyBinding::Kind::Text);
        CHECK(binding.character == c);
    }

    // With no field open the same letters are commands again.
    ActionContext closed;
    closed.tool = ToolKind::Line;
    const KeyBinding command = resolveKey(closed, charKey('r'));
    REQUIRE(command.kind == KeyBinding::Kind::Action);
    CHECK(command.action->name == "tool.rectangle");
}

TEST_CASE("a digit means nothing with no tool running") {
    // Selection is the home state and there is no placement to type a value
    // for, so the key belongs to whatever the surface wants to do with it.
    ActionContext context;
    context.tool = ToolKind::Select;
    CHECK(resolveKey(context, digitKey(4)).kind == KeyBinding::Kind::None);
    CHECK(resolveKey(context, charKey('.')).kind == KeyBinding::Kind::None);
}

TEST_CASE("typing a value through the keyboard policy reaches the document") {
    // End to end over the resolver, which is the path the shell takes: a bare
    // digit has to open a field and land a value even with an offer generated.
    Document doc;
    const EntityId a = paroculus::test::addPoint(doc, 0.0, 0.0);
    const EntityId b = paroculus::test::addPoint(doc, 100.0, 62.0);
    paroculus::test::addSegment(doc, a, b);

    UndoJournal journal;
    Session session(doc, journal);
    session.setViewport(registryViewport());
    session.snapPolicy().gridEnabled = false;
    session.setTool(ToolKind::Line);

    auto at = [&](Point p) { return session.viewport().view.toScreen(p); };
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{200.0, 0.0}),
                                    session.viewport().view));
    session.handle(PointerEvent::at(PointerAction::Press, at(Point{200.0, 0.0}),
                                    session.viewport().view, Button::Left));
    // Alongside the slanted segment, so a parallel is offered.
    session.handle(PointerEvent::at(PointerAction::Move, at(Point{300.0, 61.0}),
                                    session.viewport().view));
    REQUIRE_FALSE(session.presentation().offers().empty());

    auto press = [&](const KeyStroke &stroke) {
        const KeyBinding binding = resolveKey(contextOf(session), stroke);
        if(binding.kind == KeyBinding::Kind::Text) session.type(binding.character);
        if(binding.kind == KeyBinding::Kind::Action) {
            invokeAction(session, binding.action->name, binding.arguments);
        }
    };
    press(digitKey(4));
    press(digitKey(5));
    CHECK(session.presentation().numericText == "45");
    session.handle(Key::Enter);

    const Pose pose = session.pose();
    std::vector<Point> placed;
    for(const EntityRecord &r : doc.entities().records()) {
        if(r.kind == EntityKind::Point && r.id != a && r.id != b) {
            placed.push_back(*pose.point(r.id));
        }
    }
    REQUIRE(placed.size() == 2);
    CHECK(std::hypot(placed[1].x - placed[0].x, placed[1].y - placed[0].y) ==
          doctest::Approx(45.0));
}
