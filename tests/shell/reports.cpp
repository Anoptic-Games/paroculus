// The U3 interchange surface, exercised through the workspace the QML binds.
//
// The reports maturity — click-to-select payloads and an entry per producer —
// the export loss report and its short-write surfacing, and the import count
// parity between the dialog path and the headless trace. Each is shell logic
// pushed below the QML, so the offscreen QGuiApplication translation.cpp stands
// up is enough: a workspace is a QObject, but nothing here needs a window.
//
// The sweep is the load-bearing test: each no-silent-changes producer, when it
// fires, must emit exactly one entry — not zero (the silent change the policy
// exists to rule out) and not a stale re-emission of another producer's still-
// standing fields. Driven producer by producer, each isolated by syncing the
// report baseline after its setup so the delta is the producer's alone.
//
// No main and no DOCTEST_CONFIG_IMPLEMENT: translation.cpp owns both for this
// runner.
#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <QObject>
#include <QVariantList>

#include "core/document.h"
#include "core/svg.h"
#include "shell/models/reports.h"
#include "shell/workspace.h"
#include "shell/workspaces.h"

using namespace paroculus;

namespace {

EntityId addPoint(Workspace &ws, double x, double y) {
    EntityRecord p;
    p.kind = EntityKind::Point;
    p.seeds = {x, y};
    ws.journal().applyStep(ws.document(), "p", AddRecord<EntityRecord>{p});
    return ws.document().entities().records().back().id;
}

EntityId addSegment(Workspace &ws, EntityId a, EntityId b) {
    EntityRecord s;
    s.kind = EntityKind::Segment;
    s.points[0] = a;
    s.points[1] = b;
    ws.journal().applyStep(ws.document(), "s", AddRecord<EntityRecord>{s});
    return ws.document().entities().records().back().id;
}

ConstraintId addConstraint(Workspace &ws, ConstraintKind kind, std::vector<EntityId> operands,
                           Slot value = Slot()) {
    ConstraintRecord c;
    c.kind = kind;
    for(size_t i = 0; i < operands.size() && i < c.operands.size(); i++) c.operands[i] = operands[i];
    c.value = value;
    ws.journal().applyStep(ws.document(), "c", AddRecord<ConstraintRecord>{c});
    return ws.document().constraints().records().back().id;
}

// A segment on the base layer, selected, refreshed. The common styleable target.
EntityId buildSelectedSegment(Workspace &ws) {
    const EntityId a = addPoint(ws, 0.0, 0.0);
    const EntityId b = addPoint(ws, 40.0, 0.0);
    const EntityId seg = addSegment(ws, a, b);
    ws.session().refresh();
    ws.session().select({seg});
    return seg;
}

// Syncs the report baseline to the current session state and returns the reports
// row count, so a following producer's emission is measured as a delta against a
// document whose setup has already been folded into the baseline.
int settle(Workspace &ws) {
    ws.session().refresh();
    ws.notifyChanged();
    return ws.reports()->rowCount();
}

std::string corpusImport() {
    std::ifstream in(std::string(PAROCULUS_CORPUS_DIR) + "/import.svg");
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("the reports model carries a click-to-select payload and reports selectability") {
    ReportsModel model;
    // An entry that names records is selectable; one that names none is not.
    model.append(QStringLiteral("Edges cross"), QString(), QVariantList{3, 7});
    model.append(QStringLiteral("Deleted 2 shapes"), QStringLiteral("deletion"));

    const QModelIndex crossing = model.index(0);
    CHECK(model.data(crossing, ReportsModel::SelectableRole).toBool());
    CHECK(model.data(crossing, ReportsModel::EntitiesRole).toList() == QVariantList{3, 7});
    CHECK(model.roleNames().value(ReportsModel::EntitiesRole) == QByteArray("entities"));

    const QModelIndex deletion = model.index(1);
    CHECK_FALSE(model.data(deletion, ReportsModel::SelectableRole).toBool());
    CHECK(model.data(deletion, ReportsModel::EntitiesRole).toList().isEmpty());
}

TEST_CASE("reports sweep: deletion emits exactly one entry naming no live record") {
    Workspace ws;
    const EntityId seg = buildSelectedSegment(ws);
    (void)seg;
    const int before = settle(ws);
    ws.deleteSelection();
    CHECK(ws.reports()->rowCount() == before + 1);
    // A deletion's records are gone, so it is not click-to-select.
    const QModelIndex row = ws.reports()->index(before);
    CHECK_FALSE(ws.reports()->data(row, ReportsModel::SelectableRole).toBool());
}

TEST_CASE("reports sweep: duplicate emits exactly one entry") {
    Workspace ws;
    buildSelectedSegment(ws);
    const int before = settle(ws);
    REQUIRE(ws.run(QStringLiteral("edit.duplicate")));
    CHECK(ws.reports()->rowCount() == before + 1);
}

TEST_CASE("reports sweep: retarget-axes emits one entry that names its cluster frame") {
    Workspace ws;
    const EntityId seg = buildSelectedSegment(ws);
    addConstraint(ws, ConstraintKind::Horizontal, {seg});
    ws.session().refresh();
    ws.session().select({seg});
    const int before = settle(ws);
    REQUIRE(ws.run(QStringLiteral("relation.retarget-axes")));
    REQUIRE(ws.reports()->rowCount() == before + 1);
    // The retarget names the frame it created, so the entry is click-to-select.
    const QModelIndex row = ws.reports()->index(before);
    CHECK(ws.reports()->data(row, ReportsModel::SelectableRole).toBool());
    CHECK_FALSE(ws.reports()->data(row, ReportsModel::EntitiesRole).toList().isEmpty());
}

TEST_CASE("reports sweep: a forked style emits exactly one entry") {
    // A style shared beyond the selection forks on a direct edit. Build two
    // segments, give both one shared style, then recolour only one: the style is
    // shared with the segment outside the selection, so the edit forks.
    Workspace ws;
    const EntityId a0 = addPoint(ws, 0.0, 0.0);
    const EntityId a1 = addPoint(ws, 10.0, 0.0);
    const EntityId segA = addSegment(ws, a0, a1);
    const EntityId b0 = addPoint(ws, 0.0, 10.0);
    const EntityId b1 = addPoint(ws, 10.0, 10.0);
    const EntityId segB = addSegment(ws, b0, b1);
    ws.session().refresh();

    ws.session().select({segA, segB});
    QVariantMap red;
    red[QStringLiteral("color")] = 4278190335.0;  // 0xff0000ff
    REQUIRE(ws.run(QStringLiteral("style.set-stroke"), red));
    REQUIRE(ws.namedStyles().size() == 1);  // one shared style over both

    ws.session().select({segA});
    const int before = settle(ws);
    QVariantMap blue;
    blue[QStringLiteral("color")] = 4278255360.0;  // 0xff00ff00, a different colour
    REQUIRE(ws.run(QStringLiteral("style.set-stroke"), blue));
    CHECK(ws.reports()->rowCount() == before + 1);
    CHECK(ws.namedStyles().size() == 2);  // the fork gave segA its own style
}

TEST_CASE("reports sweep: a refused set-value emits one entry naming the conflict") {
    // Two points pinned in place with a driving distance between them. Setting the
    // distance to a value the pins cannot allow refuses and offers the reference —
    // the downgrade the const preview never touches, so a present optional is
    // always an edit that refused.
    Workspace ws;
    const EntityId a = addPoint(ws, 0.0, 0.0);
    const EntityId b = addPoint(ws, 100.0, 0.0);
    addConstraint(ws, ConstraintKind::Pin, {a});
    addConstraint(ws, ConstraintKind::Pin, {b});
    const ConstraintId dist =
        addConstraint(ws, ConstraintKind::PointPointDistance, {a, b}, Slot(100.0));
    ws.session().refresh();

    ws.selectRelation(dist.value());
    const int before = settle(ws);
    QVariantMap set;
    set[QStringLiteral("value")] = 200.0;
    // The refusal returns false — the document is byte-identical — but the report
    // is the point: the no-silent-change is that a value was refused.
    ws.run(QStringLiteral("relation.set-value"), set);
    REQUIRE(ws.reports()->rowCount() == before + 1);
    const QModelIndex row = ws.reports()->index(before);
    CHECK(ws.reports()->data(row, ReportsModel::SelectableRole).toBool());
}

TEST_CASE("reports sweep: hidden influence emits one entry naming the hidden geometry") {
    // A distance holds a visible point to a hidden one. Moving the visible one
    // makes the solve move the hidden one — the influence named rather than
    // merely happening.
    Workspace ws;
    const EntityId a = addPoint(ws, 0.0, 0.0);
    const EntityId b = addPoint(ws, 100.0, 0.0);
    addConstraint(ws, ConstraintKind::PointPointDistance, {a, b}, Slot(100.0));
    // Put a on a new layer and hide it.
    REQUIRE(ws.run(QStringLiteral("layer.new")));
    const int layerId = ws.layers()[0].toMap()[QStringLiteral("id")].toInt();
    ws.session().select({a});
    QVariantMap assign;
    assign[QStringLiteral("layer")] = layerId;
    REQUIRE(ws.run(QStringLiteral("layer.assign"), assign));
    QVariantMap hide;
    hide[QStringLiteral("layer")] = layerId;
    REQUIRE(ws.run(QStringLiteral("layer.hide"), hide));

    const int before = settle(ws);
    // Move the visible end so the solve has to move the hidden one.
    EntityRecord moved = *ws.document().entities().find(b);
    moved.seeds[0] = 200.0;
    ws.journal().applyStep(ws.document(), "move", SetRecord<EntityRecord>{moved});
    ws.session().refresh();
    REQUIRE_FALSE(ws.session().presentation().hiddenInfluences.empty());
    ws.notifyChanged();
    REQUIRE(ws.reports()->rowCount() == before + 1);
    const QModelIndex row = ws.reports()->index(before);
    CHECK(ws.reports()->data(row, ReportsModel::SelectableRole).toBool());
    // It names the hidden point.
    CHECK(ws.reports()->data(row, ReportsModel::EntitiesRole).toList().contains(int(a.value())));
}

TEST_CASE("reports sweep: two identically-counted deletions each emit an entry") {
    // The snapshot must key deletion on a per-operation serial, not the count
    // fields, or a second lone-point deletion compares equal to the first and is
    // swallowed — the silent change the whole surface exists to rule out.
    Workspace ws;
    const EntityId a = addPoint(ws, 0.0, 0.0);
    const EntityId b = addPoint(ws, 10.0, 0.0);
    ws.session().refresh();

    settle(ws);
    CHECK(ws.reports()->rowCount() == 0);
    ws.session().select({a});
    ws.deleteSelection();
    CHECK(ws.reports()->rowCount() == 1);
    ws.session().select({b});
    ws.deleteSelection();
    CHECK(ws.reports()->rowCount() == 2);  // not swallowed despite identical counts
}

TEST_CASE("reports sweep: a rotate that only moves geometry emits a Moved entry") {
    Workspace ws;
    const EntityId seg = buildSelectedSegment(ws);
    (void)seg;
    const int before = settle(ws);
    QVariantMap rot;
    rot[QStringLiteral("degrees")] = 90.0;
    REQUIRE(ws.run(QStringLiteral("transform.rotate"), rot));
    REQUIRE(ws.reports()->rowCount() == before + 1);
    CHECK(ws.reports()->latest().contains(QStringLiteral("Moved")));
}

TEST_CASE("reports sweep: distribute reports as distributed, never as copied") {
    // Distribute adds construction gap segments and copies nothing; a report that
    // read "Copied N shapes" off the entities count would be actively wrong.
    Workspace ws;
    const EntityId p0 = addPoint(ws, 0.0, 0.0);
    const EntityId p1 = addPoint(ws, 10.0, 1.0);
    const EntityId p2 = addPoint(ws, 20.0, 0.0);
    ws.session().refresh();
    ws.session().select({p0, p1, p2});
    const int before = settle(ws);
    REQUIRE(ws.run(QStringLiteral("relation.distribute")));
    REQUIRE(ws.reports()->rowCount() == before + 1);
    CHECK(ws.reports()->latest().contains(QStringLiteral("Distributed")));
    CHECK_FALSE(ws.reports()->latest().contains(QStringLiteral("Copied")));
}

TEST_CASE("reports sweep: mirror reports as mirrored") {
    // A segment as the axis and a point off it: the point reflects, and the report
    // names the operation rather than calling it a plain copy.
    Workspace ws;
    const EntityId a = addPoint(ws, 0.0, 0.0);
    const EntityId b = addPoint(ws, 0.0, 20.0);
    const EntityId axis = addSegment(ws, a, b);
    const EntityId c = addPoint(ws, 10.0, 10.0);
    ws.session().refresh();
    ws.session().select({axis, c});
    const int before = settle(ws);
    REQUIRE(ws.run(QStringLiteral("relation.mirror")));
    REQUIRE(ws.reports()->rowCount() == before + 1);
    CHECK(ws.reports()->latest().contains(QStringLiteral("Mirrored")));
}

TEST_CASE("navigation does not post a report: undo and redo of a reported edit stay quiet") {
    // A history walk is navigation, not an edit — the event was reported when the
    // edit first ran, and re-posting it on the walk would attribute it to a
    // gesture that made no change.
    Workspace ws;
    buildSelectedSegment(ws);
    const int before = settle(ws);
    REQUIRE(ws.run(QStringLiteral("edit.duplicate")));
    const int afterEdit = ws.reports()->rowCount();
    CHECK(afterEdit == before + 1);

    ws.undo();
    CHECK(ws.reports()->rowCount() == afterEdit);  // undo adds nothing
    ws.redo();
    CHECK(ws.reports()->rowCount() == afterEdit);  // nor redo
}

TEST_CASE("selectReported lands the selection on the records the entry names") {
    Workspace ws;
    const EntityId a = addPoint(ws, 0.0, 0.0);
    const EntityId b = addPoint(ws, 40.0, 0.0);
    const EntityId seg = addSegment(ws, a, b);
    ws.session().refresh();

    ws.selectReported(QVariantList{int(seg.value())}, QVariantList{});
    REQUIRE(ws.session().selection().items().size() == 1);
    CHECK(ws.session().selection().items().front() == seg);

    // An empty payload clears the selection.
    ws.selectReported(QVariantList{}, QVariantList{});
    CHECK(ws.session().selection().empty());
}

TEST_CASE("export writes, reports its loss, and never leaks the background") {
    Workspace ws;
    buildSelectedSegment(ws);
    ws.setBackground(QStringLiteral("#ff803010"));
    const int before = settle(ws);

    const QString path = QStringLiteral("/tmp/paroculus-u3-export.svg");
    std::remove(path.toStdString().c_str());
    REQUIRE(ws.exportSvg(path, 8.0, 4));
    // One loss report entry, the same surface every producer reaches.
    CHECK(ws.reports()->rowCount() == before + 1);

    std::ifstream in(path.toStdString());
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string svg = buffer.str();
    CHECK_FALSE(svg.empty());
    // The viewing aid stays a viewing aid: the background is not in the bytes.
    CHECK(svg.find("803010") == std::string::npos);
    std::remove(path.toStdString().c_str());
}

TEST_CASE("a short write is a failed export, surfaced not swallowed") {
    // A destination whose parent is a regular file cannot be created or opened, so
    // the write fails at the open — the loud failure a truncated file would hide.
    Workspace ws;
    buildSelectedSegment(ws);

    const QString blocker = QStringLiteral("/tmp/paroculus-u3-blocker");
    { std::ofstream(blocker.toStdString()) << "x"; }
    const QString path = blocker + QStringLiteral("/under-a-file.svg");

    int failures = 0;
    QObject::connect(&ws, &Workspace::exportFailed, &ws, [&](const QString &) { failures++; });
    const int before = ws.reports()->rowCount();
    CHECK_FALSE(ws.exportSvg(path, 8.0, 4));
    CHECK(failures == 1);
    // A failed export logs no success report.
    CHECK(ws.reports()->rowCount() == before);
    std::remove(blocker.toStdString().c_str());
}

TEST_CASE("a full device is a short write, caught on the close, not on the open") {
    // The blocker test above fails at the open; this one opens fine and fails on
    // the write/flush — the end-to-end check (out.close(); if(!out)) the plan asks
    // be exercised. Guarded, since /dev/full is not present in every environment.
    std::ofstream probe("/dev/full");
    if(!probe.is_open()) return;  // no /dev/full here; nothing to assert
    probe.close();

    Workspace ws;
    buildSelectedSegment(ws);
    int failures = 0;
    QObject::connect(&ws, &Workspace::exportFailed, &ws, [&](const QString &) { failures++; });
    const int before = ws.reports()->rowCount();
    CHECK_FALSE(ws.exportSvg(QStringLiteral("/dev/full"), 8.0, 4));
    CHECK(failures == 1);
    CHECK(ws.reports()->rowCount() == before);  // a failed export logs no success
}

TEST_CASE("the export loss report describes what the bake would cost") {
    Workspace ws;
    buildSelectedSegment(ws);
    const QVariantMap report = ws.exportReport(8.0, 4);
    // A lone segment is one stroke, no fills, and drops nothing structural — so
    // the export is lossless. The wording is the shared summarizer's.
    CHECK(report[QStringLiteral("survived")].toString() == QStringLiteral("1 strokes, 0 fills"));
    CHECK_FALSE(report[QStringLiteral("lossy")].toBool());
}

TEST_CASE("import parity: the dialog path traces the same counts as the headless one") {
    const std::string svg = corpusImport();
    REQUIRE_FALSE(svg.empty());
    const SvgImport headless = readSvg(svg);

    WorkspaceManager manager(nullptr);
    manager.startup();
    const int before = manager.count();
    // Write the corpus SVG to a temp file the manager can open, since importSvg
    // reads a path exactly as the dialog hands it one.
    const std::string path = "/tmp/paroculus-u3-import.svg";
    { std::ofstream(path) << svg; }
    REQUIRE(manager.importSvg(QStringLiteral("/tmp/paroculus-u3-import.svg")));

    // A fresh workspace with the trace, activated. The startup tab was pristine, so
    // it is reused rather than left beside the import.
    Workspace *imported = manager.active();
    REQUIRE(imported != nullptr);
    CHECK(manager.count() == before);  // reused the pristine scratch tab

    // The trace is dirty, unsaved content — not a pristine tab to be reused away.
    CHECK(imported->dirty());
    CHECK(imported->filePath().isEmpty());

    // Exactly one report entry, and it carries the same counts the headless trace
    // reports — the parity the plan asks for.
    REQUIRE(imported->reports()->rowCount() == 1);
    const QString text = imported->reports()->latest();
    CHECK(text.contains(QString::number(headless.traced)));
    CHECK(text.contains(QString::number(headless.skipped)));
    std::remove(path.c_str());
}

TEST_CASE("importing an SVG with no supported geometry fails rather than spawning a stray tab") {
    // An empty (or all-unsupported) SVG traces zero: a failed import, surfaced as a
    // diagnostic, not a new dirty blank workspace beside the user's work.
    const std::string path = "/tmp/paroculus-u3-empty.svg";
    { std::ofstream(path) << "<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>"; }

    WorkspaceManager manager(nullptr);
    manager.startup();
    const int before = manager.count();

    int failed = 0;
    QObject::connect(&manager, &WorkspaceManager::openFailed, &manager,
                     [&](const QString &, const QString &, int) { failed++; });
    CHECK_FALSE(manager.importSvg(QStringLiteral("/tmp/paroculus-u3-empty.svg")));
    CHECK(failed == 1);
    CHECK(manager.count() == before);  // no stray tab, nothing adopted
    std::remove(path.c_str());
}

TEST_CASE("replay smoke: a recorded script plays into a fresh workspace with progress") {
    // The developer replay path and its overlay projections, under offscreen QPA.
    // The step timer needs an event loop to advance, and the test runs none, so the
    // replay is caught at its start: playing, at step 0, with the recorded total.
    const QString scriptPath = QStringLiteral("/tmp/paroculus-u3-replay.paro");
    {
        Workspace rec;
        rec.startRecording(scriptPath);
        QVariantMap p;
        p[QStringLiteral("name")] = QStringLiteral("a");
        p[QStringLiteral("value")] = 1.0;
        REQUIRE(rec.run(QStringLiteral("parameter.create"), p));
        rec.stopRecording();
    }

    WorkspaceManager manager(nullptr);
    manager.startup();
    REQUIRE(manager.replayScript(scriptPath));
    Workspace *w = manager.active();
    REQUIRE(w != nullptr);
    CHECK(w->playing());  // the `replaying` QML property reads playing()
    CHECK(w->replayTotal() >= 1);
    CHECK(w->replayStep() == 0);
    std::remove(scriptPath.toStdString().c_str());
}
