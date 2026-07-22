// The deep-panel projections, exercised through the workspace the QML binds.
//
// Each panel is a projection of a session query, and the projection is where a
// name that spells a number could be read as one or a fork could go uncounted, so
// it is worth a headless check under the offscreen QGuiApplication the runner
// stands up. The QML above is layout over exactly these maps.
//
// No main and no DOCTEST_CONFIG_IMPLEMENT: translation.cpp owns both for this
// runner.
#include <doctest/doctest.h>

#include <QVariantMap>

#include "core/document.h"
#include "core/pose.h"
#include "core/svg.h"
#include "interact/drag.h"
#include "render/view.h"
#include "shell/workspace.h"

using namespace paroculus;

namespace {

// A segment on the base layer, selected, so the style projections have a
// styleable target. Built through the journal, the way the shell mutates.
EntityId buildSegment(Workspace &ws) {
    EntityRecord a;
    a.kind = EntityKind::Point;
    a.seeds = {0.0, 0.0};
    ws.journal().applyStep(ws.document(), "p", AddRecord<EntityRecord>{a});
    const EntityId pa = ws.document().entities().records().back().id;
    EntityRecord b;
    b.kind = EntityKind::Point;
    b.seeds = {40.0, 0.0};
    ws.journal().applyStep(ws.document(), "p", AddRecord<EntityRecord>{b});
    const EntityId pb = ws.document().entities().records().back().id;
    EntityRecord s;
    s.kind = EntityKind::Segment;
    s.points[0] = pa;
    s.points[1] = pb;
    ws.journal().applyStep(ws.document(), "s", AddRecord<EntityRecord>{s});
    const EntityId seg = ws.document().entities().records().back().id;
    ws.session().refresh();
    ws.session().select({seg});
    return seg;
}

}  // namespace

TEST_CASE("run() routes a string argument to the text channel by the action schema") {
    Workspace ws;
    // parameter.create's name is a string. A value that spells a number must
    // still reach the string channel, which only the schema can decide.
    QVariantMap args;
    args[QStringLiteral("name")] = QStringLiteral("2024");
    args[QStringLiteral("value")] = 8.0;
    CHECK(ws.run(QStringLiteral("parameter.create"), args));

    const QVariantList params = ws.parameters();
    REQUIRE(params.size() == 1);
    CHECK(params[0].toMap()[QStringLiteral("name")].toString() == QStringLiteral("2024"));
    CHECK(params[0].toMap()[QStringLiteral("value")].toDouble() == doctest::Approx(8.0));
}

TEST_CASE("the appearance projection forks a style and reports it") {
    Workspace ws;
    buildSegment(ws);

    CHECK(ws.appearance()[QStringLiteral("any")].toBool());
    CHECK(ws.appearance()[QStringLiteral("entities")].toInt() == 1);
    CHECK(ws.namedStyles().isEmpty());

    QVariantMap stroke;
    stroke[QStringLiteral("color")] = 4278190335.0;  // 0xff0000ff
    CHECK(ws.run(QStringLiteral("style.set-stroke"), stroke));

    // The unstyled segment forked its own style.
    const QVariantList styles = ws.namedStyles();
    REQUIRE(styles.size() == 1);
    CHECK(styles[0].toMap()[QStringLiteral("usage")].toInt() == 1);
    CHECK(ws.appearance()[QStringLiteral("strokeColor")].toString() == QStringLiteral("#ff0000ff"));
}

TEST_CASE("the parameters projection carries the cycle check and freeze delete") {
    Workspace ws;
    QVariantMap a;
    a[QStringLiteral("name")] = QStringLiteral("a");
    a[QStringLiteral("value")] = 2.0;
    CHECK(ws.run(QStringLiteral("parameter.create"), a));
    const int aId = ws.parameters()[0].toMap()[QStringLiteral("id")].toInt();

    // A self-reference is a cycle, caught inline.
    CHECK(ws.parameterWouldCycle(aId, QStringLiteral("a")));
    CHECK_FALSE(ws.parameterWouldCycle(aId, QStringLiteral("2 + 3")));

    // Setting a constant through the expression channel evaluates.
    QVariantMap set;
    set[QStringLiteral("id")] = aId;
    set[QStringLiteral("expression")] = QStringLiteral("2 + 3");
    CHECK(ws.run(QStringLiteral("parameter.set"), set));
    CHECK(ws.parameters()[0].toMap()[QStringLiteral("value")].toDouble() == doctest::Approx(5.0));
}

TEST_CASE("the history projection tracks the journal position and walks it") {
    Workspace ws;
    QVariantMap a;
    a[QStringLiteral("name")] = QStringLiteral("a");
    a[QStringLiteral("value")] = 1.0;
    ws.run(QStringLiteral("parameter.create"), a);
    QVariantMap b;
    b[QStringLiteral("name")] = QStringLiteral("b");
    b[QStringLiteral("value")] = 2.0;
    ws.run(QStringLiteral("parameter.create"), b);

    CHECK(ws.history().size() == 2);
    CHECK(ws.historyPosition() == 2);

    // Walk back to the start through the ordinary undo path.
    ws.walkHistory(0);
    CHECK(ws.historyPosition() == 0);
    CHECK(ws.parameters().isEmpty());

    // And forward again.
    ws.walkHistory(2);
    CHECK(ws.historyPosition() == 2);
    CHECK(ws.parameters().size() == 2);
}

TEST_CASE("the layers projection carries the active layer and per-row counts") {
    // The implicit null base layer has no record, so the panel lists the real
    // layers only — the same as U0. A new layer starts empty; assigning the
    // selection moves its count onto it, and activating it lights the row.
    Workspace ws;
    buildSegment(ws);  // three entities on the base layer
    CHECK(ws.run(QStringLiteral("layer.new"), {}));
    REQUIRE(ws.layers().size() == 1);
    const int layerId = ws.layers()[0].toMap()[QStringLiteral("id")].toInt();
    CHECK(ws.layers()[0].toMap()[QStringLiteral("count")].toInt() == 0);
    CHECK_FALSE(ws.layers()[0].toMap()[QStringLiteral("active")].toBool());

    // assign moves exactly the selection — the segment, not its endpoints — so
    // the new layer gains one entity.
    QVariantMap assign;
    assign[QStringLiteral("layer")] = layerId;
    CHECK(ws.run(QStringLiteral("layer.assign"), assign));
    CHECK(ws.layers()[0].toMap()[QStringLiteral("count")].toInt() == 1);

    QVariantMap activate;
    activate[QStringLiteral("layer")] = layerId;
    CHECK(ws.run(QStringLiteral("layer.activate"), activate));
    CHECK(ws.activeLayer() == layerId);
    CHECK(ws.layers()[0].toMap()[QStringLiteral("active")].toBool());
}

TEST_CASE("U2 presentation toggles mutate no document and record no step") {
    // Inspect mode, extensions, frames, grid and background are presentation:
    // they never reach the document or the journal, so a script is unaffected and
    // the determinism property holds. This is the byte-level guarantee.
    Workspace ws;
    buildSegment(ws);
    const quint64 revision = ws.journal().revision();
    const std::string before = ws.serializeDocument();

    ws.toggleInspectMode();
    ws.setExtensions(true);
    ws.setShowAllFrames(true);
    ws.setGridVisible(false);
    ws.setBackground(QStringLiteral("#ff2a3d55"));
    ws.setGlyphDensity(2.0);

    CHECK(ws.inspectMode());
    CHECK(ws.extensions());
    CHECK(ws.hasBackground());
    CHECK(ws.journal().revision() == revision);
    CHECK(ws.serializeDocument() == before);
}

TEST_CASE("the background colour never reaches the exported bytes") {
    Workspace ws;
    buildSegment(ws);
    ws.setBackground(QStringLiteral("#ff803010"));
    REQUIRE(ws.hasBackground());
    // The bake and the SVG writer never see the background — it is a viewing aid.
    const std::string svg = writeSvg(ws.document(), ws.session().pose());
    CHECK(svg.find("803010") == std::string::npos);
}

TEST_CASE("the HUD projections read the overlay and the direction classes") {
    Workspace ws;
    const EntityId seg = buildSegment(ws);
    ws.run(QStringLiteral("constrain.horizontal"), {});
    (void)seg;

    // A viewport, since the glyph readout is against the viewport the overlay
    // draws into — a headless workspace has none until one is set.
    Viewport viewport;
    viewport.view = defaultView(800, 600);
    viewport.width = 800;
    viewport.height = 600;
    ws.session().setViewport(viewport);

    // One segment is one direction class; the horizontal it now carries is a
    // relation the overlay can show.
    CHECK(ws.directionClassCount() == 1);
    const QVariantMap readout = ws.glyphReadout();
    CHECK(readout[QStringLiteral("total")].toInt() >= 1);
    CHECK(readout[QStringLiteral("shown")].toInt() <= readout[QStringLiteral("total")].toInt());
}

TEST_CASE("opening a document clears transient inspect mode") {
    // Inspect mode belongs to the session being replaced, not the document
    // arriving: a file opened into a tab left in inspect mode must land editable.
    Workspace ws;
    buildSegment(ws);
    const std::string text = ws.serializeDocument();
    ws.setInspectMode(true);
    REQUIRE(ws.inspectMode());
    ws.loadFrom(text, QStringLiteral("/tmp/inspect-reset.paro"));
    CHECK_FALSE(ws.inspectMode());
}
