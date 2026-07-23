// Workspace lifecycle, dirty tracking, and the sidecar-independence property.
//
// These reach the shell logic that was pushed below the QML — a workspace is a
// QObject, but nothing here needs the event loop or a window, so the offscreen
// QGuiApplication translation.cpp stands up is enough. The determinism property
// the spec states as a test lives here because it is the workspace that keeps the
// document and its view sidecar apart: opening with or without a sidecar and
// editing identically must produce byte-identical document bytes.
//
// No main and no DOCTEST_CONFIG_IMPLEMENT: translation.cpp owns both for this
// runner.
#include <doctest/doctest.h>

#include "core/document.h"
#include "core/persist.h"
#include "shell/models/reports.h"
#include "shell/workspace.h"
#include "shell/workspaces.h"

using namespace paroculus;

namespace {

// A small serialized document to load: two points, enough to have bytes worth
// comparing and a revision worth bumping. Built through the command layer, the
// only way a document is meant to be mutated.
std::string twoPointDocument() {
    Document doc;
    EntityRecord a;
    a.kind = EntityKind::Point;
    a.seeds = {1.5, -2.25};
    doc.apply(AddRecord<EntityRecord>{a});
    EntityRecord b;
    b.kind = EntityKind::Point;
    b.seeds = {10.0, 4.0};
    doc.apply(AddRecord<EntityRecord>{b});
    return serialize(doc);
}

EntityId addPointVia(Workspace &ws, double x, double y) {
    EntityRecord p;
    p.kind = EntityKind::Point;
    p.seeds = {x, y};
    ws.journal().applyStep(ws.document(), "add point", AddRecord<EntityRecord>{p});
    return ws.document().entities().records().back().id;
}

}  // namespace

TEST_CASE("the reports model appends, counts, exposes its roles, and reports its latest") {
    ReportsModel model;
    CHECK(model.rowCount() == 0);
    CHECK(model.latest().isEmpty());

    model.append(QStringLiteral("Deleted 2 shapes, 1 relations"), QStringLiteral("deletion"));
    model.append(QStringLiteral("Copied 3 shapes"), QStringLiteral("structure"));
    CHECK(model.rowCount() == 2);
    CHECK(model.latest() == QStringLiteral("Copied 3 shapes"));

    const QModelIndex second = model.index(1);
    CHECK(model.data(second, ReportsModel::TextRole).toString() == QStringLiteral("Copied 3 shapes"));
    CHECK(model.data(second, ReportsModel::KindRole).toString() == QStringLiteral("structure"));
    // The role names the QML delegate binds by.
    CHECK(model.roleNames().value(ReportsModel::TextRole) == QByteArray("text"));

    model.clear();
    CHECK(model.rowCount() == 0);
    CHECK(model.latest().isEmpty());
}

TEST_CASE("a workspace exposes a reports model that starts empty") {
    Workspace ws;
    REQUIRE(ws.reports() != nullptr);
    CHECK(ws.reports()->rowCount() == 0);
}

TEST_CASE("a fresh workspace is clean, untitled, and empty") {
    Workspace ws;
    CHECK_FALSE(ws.dirty());
    CHECK(ws.filePath().isEmpty());
    CHECK(ws.title().startsWith("untitled"));
}

TEST_CASE("loading a document round-trips its bytes and clears dirty") {
    const std::string text = twoPointDocument();
    Workspace ws;
    const LoadResult result = ws.loadFrom(text, QStringLiteral("/tmp/paroculus-test.paro"));
    REQUIRE(result.ok);

    // Loaded, not half-loaded: the bytes back out equal the bytes in.
    CHECK(ws.serializeDocument() == text);
    // A just-loaded document is the saved document, so it is clean.
    CHECK_FALSE(ws.dirty());
    CHECK(ws.filePath() == QStringLiteral("/tmp/paroculus-test.paro"));
    CHECK(ws.title() == QStringLiteral("paroculus-test.paro"));
}

TEST_CASE("a refused load leaves the workspace untouched") {
    Workspace ws;
    const std::string good = twoPointDocument();
    REQUIRE(ws.loadFrom(good, QStringLiteral("/tmp/a.paro")).ok);

    // A malformed document is refused whole; the workspace keeps what it had,
    // never a partial load wearing a valid document's interface.
    const LoadResult bad = ws.loadFrom("this is not a document", QStringLiteral("/tmp/b.paro"));
    CHECK_FALSE(bad.ok);
    CHECK(ws.serializeDocument() == good);
    CHECK(ws.filePath() == QStringLiteral("/tmp/a.paro"));
}

TEST_CASE("dirty tracks the journal revision across edit and save") {
    Workspace ws;
    ws.loadFrom(twoPointDocument(), QStringLiteral("/tmp/x.paro"));
    REQUIRE_FALSE(ws.dirty());

    addPointVia(ws, 3.0, 3.0);
    CHECK(ws.dirty());  // an edit past the saved revision

    ws.markSaved(QStringLiteral("/tmp/x.paro"));
    CHECK_FALSE(ws.dirty());  // save re-marks the current revision as clean

    addPointVia(ws, 4.0, 4.0);
    CHECK(ws.dirty());
}

TEST_CASE("a hovered relation is transient, repaint-only, and cleared by an edit") {
    // The inspector's relation hover highlights the constraint's operands on the
    // canvas. It rides highlightChanged(), not changed(): a changed() rebuilds
    // the relations list and destroys the very row being hovered, so the state
    // is repaint-only and dropped whenever a changed() would strand it.
    Workspace ws;
    const EntityId a = addPointVia(ws, -1.0, 0.0);
    const EntityId b = addPointVia(ws, 1.0, 0.0);
    ConstraintRecord c;
    c.kind = ConstraintKind::Coincident;
    c.operands[0] = a;
    c.operands[1] = b;
    ws.journal().applyStep(ws.document(), "coincide", AddRecord<ConstraintRecord>{c});
    const ConstraintId id = ws.document().constraints().records().back().id;

    int highlightSignals = 0, changedSignals = 0;
    QObject::connect(&ws, &Workspace::highlightChanged, &ws, [&] { highlightSignals++; });
    QObject::connect(&ws, &Workspace::changed, &ws, [&] { changedSignals++; });

    REQUIRE_FALSE(ws.hoveredRelation().valid());
    ws.setHoveredRelation(static_cast<int>(id.value()));
    CHECK(ws.hoveredRelation() == id);
    CHECK(highlightSignals == 1);
    CHECK(changedSignals == 0);  // repaint only — no model rebuilt under the cursor

    ws.setHoveredRelation(static_cast<int>(id.value()));  // idempotent
    CHECK(highlightSignals == 1);

    // An edit strands the hovering row before its exit fires, so notifyChanged
    // drops the highlight the same way it drops the imposition ghost.
    ws.notifyChanged();
    CHECK_FALSE(ws.hoveredRelation().valid());
    CHECK(changedSignals == 1);

    // Clear is idempotent and its own repaint.
    ws.setHoveredRelation(static_cast<int>(id.value()));
    CHECK(highlightSignals == 2);
    ws.clearHoveredRelation();
    CHECK_FALSE(ws.hoveredRelation().valid());
    CHECK(highlightSignals == 3);
    ws.clearHoveredRelation();  // no-op
    CHECK(highlightSignals == 3);
}

TEST_CASE("enabling and disabling async is idempotent and leaves the workspace not busy") {
    // The symmetric enable/disable the manager relies on to hold async on exactly
    // the active workspace. Disabling joins the workers; a second enable/disable
    // must be a no-op, and neither may leave the pump owing a result on an empty
    // document.
    Workspace ws;
    ws.enableAsync();
    ws.enableAsync();  // idempotent
    CHECK_FALSE(ws.busy());  // an empty document has nothing off-thread
    ws.disableAsync();
    ws.disableAsync();  // idempotent
    CHECK_FALSE(ws.busy());
    ws.enableAsync();  // re-enabling after a disable works
    CHECK_FALSE(ws.busy());
}

TEST_CASE("the manager keeps at least one tab and cycles and closes correctly") {
    // Lifecycle plus the active-workspace-only async invariant exercised: every
    // activate enables async on the arrival and disables it on the departure, so
    // this cycles a scheduler up and down repeatedly and must neither crash nor
    // deadlock, and must never leave the window without an active document.
    WorkspaceManager manager(nullptr);
    manager.startup();
    CHECK(manager.count() == 1);
    CHECK(manager.active() != nullptr);
    CHECK(manager.activeIndex() == 0);

    manager.newWorkspace();
    CHECK(manager.count() == 2);
    CHECK(manager.activeIndex() == 1);

    manager.cycleActive(1);  // wraps 1 -> 0
    CHECK(manager.activeIndex() == 0);
    manager.cycleActive(-1);  // wraps 0 -> 1
    CHECK(manager.activeIndex() == 1);

    Workspace *keep = manager.active();
    manager.closeWorkspace(0);  // close the non-active tab
    CHECK(manager.count() == 1);
    CHECK(manager.active() == keep);  // the active document is preserved

    manager.closeWorkspace(0);  // close the last tab
    CHECK(manager.count() == 1);         // a fresh empty tab replaces it
    CHECK(manager.active() != nullptr);  // the window is never document-less
}

TEST_CASE("the sidecar never reaches document bytes: identical edits, identical saves") {
    // The determinism property, stated as a test. Two workspaces open the same
    // document; one is given a non-default view framing as a sidecar would, the
    // other none. The same edit is applied to both, and the saved documents are
    // byte-identical — the view state is presentation and cannot leak into the
    // storage of record.
    const std::string text = twoPointDocument();

    Workspace withSidecar;
    Workspace without;
    REQUIRE(withSidecar.loadFrom(text, QStringLiteral("/tmp/a.paro")).ok);
    REQUIRE(without.loadFrom(text, QStringLiteral("/tmp/a.paro")).ok);

    // A view framing on one only, exactly what loadSidecarFrom would set.
    withSidecar.view().pan = Eigen::Vector2d(137.0, -42.0);
    withSidecar.view().zoom = 3.5;

    // The same edit through the same command on both.
    addPointVia(withSidecar, 7.0, 7.0);
    addPointVia(without, 7.0, 7.0);

    CHECK(withSidecar.serializeDocument() == without.serializeDocument());
}
