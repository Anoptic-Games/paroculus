// The new mutating vocabulary as recordings.
//
// The one property every new action owes is record → replay → record identity,
// and the new thing to pin is the string channel: a name to rename to and an
// expression to assign now ride the script, quoted, and must round-trip exactly.
// The gesture builds a real selection by clicking, never Session::select, because
// a script built from select replays with an empty selection and every action in
// it quietly refuses.
#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>

#include "interact/registry.h"
#include "interact/script.h"
#include "interact/session.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

Viewport vocabViewport() {
    Eigen::Affine2d m = Eigen::Affine2d::Identity();
    m.translate(Eigen::Vector2d(400.0, 300.0));
    m.scale(Eigen::Vector2d(1.0, -1.0));
    Viewport v;
    v.view = ViewTransform(m);
    v.width = 800.0;
    v.height = 600.0;
    return v;
}

std::unique_ptr<Session> fresh(Document &doc, UndoJournal &journal) {
    auto session = std::make_unique<Session>(doc, journal);
    session->setViewport(vocabViewport());
    return session;
}

void press(Session &session, Point p) {
    const Eigen::Vector2d screen = session.viewport().view.toScreen(p);
    session.handle(PointerEvent::at(PointerAction::Move, screen, session.viewport().view));
    session.handle(
        PointerEvent::at(PointerAction::Press, screen, session.viewport().view, Button::Left));
    session.handle(
        PointerEvent::at(PointerAction::Release, screen, session.viewport().view, Button::Left));
}

// Draws a line, selects it, and drives the new families over it: a numeric style
// edit, a named style created and renamed, and a named parameter. Every action
// records itself, string arguments included.
GestureScript vocabScript() {
    Document doc;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);
    session->snapPolicy().gridEnabled = false;

    ScriptRecorder recorder;
    session->setRecorder(&recorder);

    session->setTool(ToolKind::Line);
    press(*session, Point{-40.0, 0.0});
    press(*session, Point{40.0, 0.0});
    session->setTool(ToolKind::Select);
    press(*session, Point{40.0, 0.0});

    ActionArguments stroke;
    stroke.set("color", 4278190335.0);  // 0xff0000ff
    REQUIRE(invokeAction(*session, "style.set-stroke", stroke));

    ActionArguments create;
    create.setText("name", "outline");
    REQUIRE(invokeAction(*session, "style.create", create));

    const StyleId styleId = session->targetStyle();
    REQUIRE(styleId.valid());
    ActionArguments rename;
    rename.set("style", static_cast<double>(styleId.value()));
    rename.setText("name", "edge stroke");  // a space, so the quoting is exercised
    REQUIRE(invokeAction(*session, "style.rename", rename));

    ActionArguments fill;
    fill.set("color", 4294901760.0);  // 0xffff0000
    REQUIRE(invokeAction(*session, "style.set-fill", fill));
    ActionArguments width;
    width.set("value", 2.5);
    REQUIRE(invokeAction(*session, "style.set-stroke-width", width));

    // Parameters: a constant, an expression over it, a rename.
    ActionArguments gutter;
    gutter.setText("name", "gutter");
    gutter.set("value", 8.0);
    REQUIRE(invokeAction(*session, "parameter.create", gutter));
    ActionArguments twice;
    twice.setText("name", "twice");
    twice.set("value", 0.0);
    REQUIRE(invokeAction(*session, "parameter.create", twice));
    const ParameterId twiceId = session->targetParameter();
    REQUIRE(twiceId.valid());
    ActionArguments setExpr;
    setExpr.set("id", static_cast<double>(twiceId.value()));
    setExpr.setText("expression", "gutter * 2");  // the string channel + a reference
    REQUIRE(invokeAction(*session, "parameter.set", setExpr));
    ActionArguments prename;
    prename.set("id", static_cast<double>(twiceId.value()));
    prename.setText("name", "two gutters");
    REQUIRE(invokeAction(*session, "parameter.rename", prename));

    // Snap policy toggles: recorded, not journalled, but part of the session.
    ActionArguments grid;
    grid.set("step", 10.0);
    grid.set("enabled", 0.0);
    REQUIRE(invokeAction(*session, "snap.set-grid", grid));
    ActionArguments attract;
    attract.set("flag", 1.0);
    REQUIRE(invokeAction(*session, "snap.set-construction-attract", attract));

    // A layer created, the selection moved onto it, then renamed and activated.
    // The layer actions' applicability reads the selection's layer, so the
    // selection has to be on the layer being edited — the same reason the layers
    // panel acts on the layer under the current geometry.
    REQUIRE(invokeAction(*session, "layer.new"));
    const LayerId layer = session->topLayer();
    REQUIRE(layer.valid());
    ActionArguments lassign;
    lassign.set("layer", static_cast<double>(layer.value()));
    REQUIRE(invokeAction(*session, "layer.assign", lassign));
    ActionArguments lrename;
    lrename.set("layer", static_cast<double>(layer.value()));
    lrename.setText("name", "guides layer");
    REQUIRE(invokeAction(*session, "layer.rename", lrename));
    ActionArguments lactivate;
    lactivate.set("layer", static_cast<double>(layer.value()));
    REQUIRE(invokeAction(*session, "layer.activate", lactivate));

    GestureScript script;
    script.steps = recorder.steps();
    return script;
}

}  // namespace

TEST_CASE("vocabulary: recording the replay reproduces the script") {
    const GestureScript script = vocabScript();
    const std::string text = serializeScript(script);

    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);

    Document doc = parsed.document;
    UndoJournal journal;
    std::unique_ptr<Session> session = fresh(doc, journal);
    session->snapPolicy().gridEnabled = false;
    ScriptRecorder again;
    session->setRecorder(&again);
    replay(*session, parsed);

    GestureScript second;
    second.document = parsed.document;
    second.steps = again.steps();
    CHECK(serializeScript(second) == text);
}

TEST_CASE("vocabulary: the string arguments round-trip, not just the actions") {
    const GestureScript script = vocabScript();
    const std::string text = serializeScript(script);

    // Every action resolves, so nothing was silently dropped.
    GestureScript parsed;
    REQUIRE(parseScript(text, parsed).ok);
    std::set<std::string> names;
    bool sawGutter = false;
    bool sawSpacedName = false;
    bool sawExpression = false;
    for(const ScriptStep &step : parsed.steps) {
        if(step.kind != ScriptStep::Kind::Action) continue;
        CHECK(findAction(step.actionName) != nullptr);
        names.insert(step.actionName);
        for(const auto &[key, value] : step.textArguments) {
            if(value == "gutter") sawGutter = true;
            // A name with a space in it: it is hex-escaped in the serialized bytes
            // and must come back whole, which is the quoting the string channel
            // exists to guarantee.
            if(value == "edge stroke") sawSpacedName = true;
            if(value == "gutter * 2") sawExpression = true;
        }
    }
    // The new families that carry a string channel are all present.
    for(const char *name : {"style.set-stroke", "style.create", "style.rename",
                            "style.set-fill", "style.set-stroke-width", "parameter.create",
                            "parameter.set", "parameter.rename", "snap.set-grid",
                            "snap.set-construction-attract", "layer.rename", "layer.activate"}) {
        CAPTURE(name);
        CHECK(names.count(name) == 1);
    }
    CHECK(sawGutter);
    CHECK(sawSpacedName);
    CHECK(sawExpression);
    // The space is escaped in the bytes, never a bare literal that would split
    // the line the tokenizer reads.
    CHECK(text.find("edge stroke") == std::string::npos);
    CHECK(text.find("edge\\x20stroke") != std::string::npos);
}
