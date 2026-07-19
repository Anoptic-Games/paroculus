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
        const bool applies = a.applicable(context);
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
