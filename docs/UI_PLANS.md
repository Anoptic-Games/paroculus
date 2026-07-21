# UI implementation plan

This document turns UI_SPEC.md into modules, file-level structure, and a
sequence of bounded stages with the tests each stage must carry. UI_SPEC
governs the surface; this document governs order; PRINCIPLES.md outranks both,
and on conflict the spec and this plan are amended. The document was written
against the tree at stage 8 completion plus the stage 0-8 review's fix pass.

Ground rules, inherited from PLANS.md and unchanged: the application builds
and runs at stage exit (`nix build`, `nix flake check`, `nix run .#dev` all
green); the cumulative test corpus keeps passing; each stage ends in a
reviewable state for manual testing, after which commits are drafted by hand.
Feel-tuning stays out of stage scopes; stage U4 is the discovery window and
its findings land as policy and settings adjustments plus frozen invariants,
never as new scope.

One standing ordering rule does most of the sequencing work: registry before
surfaces. A surface may not land before the vocabulary it projects, because a
surface born against a private code path is a surface that has to be rewired
rather than rebound. This is the same rule that ordered stage 4 before stage 5,
applied three more times below.

## Architecture

### Shell module map

The shell grows from two source files to a small tree. SketchView keeps its
role — the canvas item, the event translation seam, the one painter — and
sheds everything else it accumulated as stand-in furniture.

```
src/shell/
  sketchview.{h,cpp}    the canvas item: translation, paint, viewport. Sheds
                        document ownership, the status string, the palette
                        and layer projections.
  workspace.{h,cpp}     Workspace: document, journal, session, recorder,
                        sidecar preferences, dirty state. One per tab.
  workspaces.{h,cpp}    WorkspaceManager: the open set, the active one, file
                        lifecycle (new/open/save/save-as/close), recents.
  models/               typed QAbstractListModels, one per panel: layers,
                        strip, palette, reports, history, constraints,
                        parameters, styles. Rebuilt on changed(), diffed in.
  settings.{h,cpp}      application settings and named layouts.
  sidecar.{h,cpp}       the .paro-view reader and writer.
  theme.{h,cpp}         theme tokens exposed to QML as a singleton.
  qml/
    Main.qml            window frame: menu bar, tabs, docks, status bar.
    components/         PanelHost, Panel, Toolbar, Strip, NumericEntry,
                        Palette, Tabs, StatusBar, Hud, Toast.
    panels/             Layers, Inspector, Parameters, Reports, History.
    dialogs/            file dialogs, ExportDialog, ImportDialog,
                        UnsavedPrompt.
```

Rules for this code, each the generalization of one that already holds:

- QML never calls Session. Every mutation dispatches through the existing
  `run()` / `invokeAction` entrance on the workspace; every read binds a model
  or a Q_PROPERTY projection. A panel that needed a private Session call has
  found a missing registry row, not a shortcut.
- Any arithmetic or policy a headless test should reach lives below the QML:
  in the models, in interact, or in render — the keyboard-resolution rule
  generalized. QML holds layout and binding, nothing that can be wrong in an
  interesting way.
- The panel host is a contract, not a library commitment: zones, order,
  sizes, collapsed and locked state, and the layout persistence schema are
  fixed so a docking dependency could replace the implementation without
  touching any panel.
- Sidecar and settings serialization follow persist's lexical rules —
  `std::to_chars`, never printf — for the same locale reason, though neither
  file is the storage of record for anything.

### Where the non-shell work lands

| addition | file | note |
|---|---|---|
| action metadata columns (category, description) | interact/registry | data only, two columns on the one table |
| ctrl+ binding grammar | interact/registry | resolveKey rows; unbound ctrl still resolves to nothing |
| style action family + forking rule | interact/session, registry | command shapes already exist in core |
| parameter action family | interact/session, registry | wouldCycle already answers the check |
| layer.rename, layer.activate | interact/session, registry, tools | tools stamp the active layer on what they emit |
| relation.set-value, flip-alternative | interact/session, registry | set-value checks before it drives, like the panel |
| relation.retarget-axes | core/transform + interact | a second entrance to the rewrite rotate already owns |
| snap.set-grid, snap.set-construction-attract | interact/session, registry | the two policy fields that affect edits become recorded |
| journal revision + undo/redo labels | core/undo (const queries) | labels already stored per step |
| constraint list + reverse index | core (index), interact (query) | the index selection.h already anticipates |
| direction classes | core (new direction.{h,cpp}) | union-find over the direction relations, deterministic |
| glyph fan cap, overflow mark, label text | core/glyphs + interact/glyphs | one layout, two readers, as today |
| background, extensions, axes in Adornment | render | grid precedent: shell hands down, render draws |
| resize recomposition | render/view | centre-stable, beside the composition it adjusts |

Nothing touches solve. Nothing changes the document format; the sidecar and
settings absorb every candidate field, per the spec's three-tier rule.

### Threading and async

The shell finally pays the pumpAsync bill, first, before any panel work: a
timer at frame cadence, running only while `asyncBusy()`, calling
`pumpAsync()` and repainting on a true return, never bumping the epoch. The
no-mutexes rule is untouched — models are rebuilt on the UI thread from
session projections.

Multi-workspace async needs one audit the review's suspicions list already
asks for: the solver-serialization gate protects the vendored solver's
file-global scratch, so with two workspaces' schedulers alive it must
serialize across sessions, not within one. Confirm the gate is process-global
when the second scheduler first exists, and add the two-session worker test
beside the existing async suite. Until that audit passes, async is enabled on
the active workspace only — a scheduler per hidden tab buys nothing the tab
is not showing anyway.

## Test strategy

Additions to the existing layers, cheapest first, same philosophy: below the
shell everything runs headless, and the shell's own logic is either pushed
below the QML or covered by `paroculus-shell-tests` under offscreen QPA.

- Conformance, extended: every new action headlessly invocable; the
  applicability sweep gains locked-layer coverage (the review's untested
  applicable-equals-runnable-under-lock gap closes with the first new
  predicate, not the last); a metadata sweep asserts every action carries a
  category and description.
- Record-replay, extended: one corpus script per new mutating action,
  asserting record → replay → record identity. The snap policy actions get
  the pointed version: a script that toggles grid snapping mid-drawing
  replays to the identical document, which is the property their promotion
  to actions exists to buy.
- Shell tests, extended: the ctrl chord table through `strokeOf`/`resolveKey`;
  workspace lifecycle (open, edit, save, reopen — document bytes stable);
  dirty tracking against the journal revision; sidecar round-trip; model
  rebuild and diff correctness on scripted edits.
- Determinism: the sidecar independence property — open with and without the
  sidecar, apply the same script, byte-identical saves.
- Raster, analytic: extension overlay pixels present when toggled and absent
  when not; axis overlay likewise; glyph label pixels under a loose budget;
  background color applied on canvas and absent from exported SVG bytes; the
  overflow mark drawn where the shared layout places it, and hit testing
  picking it there — the one-layout-two-readers pattern already tested for
  marks.
- Bench, informational: model rebuild time on the reference scenes, recorded
  at U0 exit so a later regression has a baseline. Not a gate; wall-clock
  gates in the sandbox are flake generators.

## Stage sequence

| Stage | Name | Delivers | Size |
|---|---|---|---|
| U0 | Shell scaffolding | pump, workspaces and tabs, panel host, menus and toolbars, models, file lifecycle | L |
| U1 | Vocabulary | the new action families and queries, ctrl rows, inspector, parameters, history, style toolbar | L |
| U2 | Canvas depth | glyph labels and overflow, axes, direction classes, inspect mode, background, HUD | M |
| U3 | Interchange | export and import dialogs with reports, developer record/replay, file-argument CLI | M |
| U4 | Discovery | user testing, feel freezing, defaults, spec amendment | S |

U0 and U1 are the large stages and each carries a midpoint checkpoint: U0
after the menu bar, toolbars and tabs are live over the existing table; U1
after the mutating vocabulary is in and swept, before the deep panels bind.

### Stage U0 — shell scaffolding

Goal: the production window frame exists, every current capability rehomes
into it, and the shell honors the async contract. Interact changes are
limited to data and read-only queries; no new mutating vocabulary.

Scope, in build order:

1. The pump. Timer wiring per the contract on pumpAsync's declaration,
   `enableAsyncSolving` with the existing threshold policy, the busy glyph in
   the status bar, active workspace only. The cross-session gate audit is
   noted on the scheduler work but not blocking here, since one scheduler
   exists at a time.
2. Workspace and WorkspaceManager. The document–journal–session triple moves
   out of SketchView into Workspace; SketchView binds one workspace and keeps
   only translation, paint and view. The pendingScript handoff moves to the
   manager; the demo document leaves the constructor (a new tab is an empty
   document; demoDocument remains for --selftest); a positional FILE argument
   opens a document at launch. Tabs in QML over the manager, dirty dots from
   the journal revision, Ctrl+Tab cycling, close with the unsaved prompt.
3. Action metadata. The category and description columns land in the
   registry as data, with the conformance sweep asserting completeness —
   pulled forward from U1 because the menu bar arrives in this stage and a
   shell-side grouping list would be the drifting second copy the registry
   exists to prevent.
4. Menu bar, tools toolbar, constraints toolbar, status bar. All projections:
   menus group by category, display bindings, dim by applicability;
   the constraints toolbar projects the imposition rows by family with the
   strengths flyout and hover ghosting through the existing preview path;
   parameterized rows open the numeric entry pending rather than executing.
   The tool options row replaces the tool fragment of the status string.
5. Panel host. Three zones, drag reorder, collapse, close, the window lock,
   View ▸ Panels; named layouts saved to settings, default layout in code,
   reset. Layers panel content at this stage is today's read-only projection
   plus the visible and lock toggles routed through their existing actions —
   the expressive upgrade waits for U1's rename and activate vocabulary.
6. Typed models and the dismantling. Layers, strip, palette, reports models;
   the status string's fragments rehome per the spec's mapping table; toasts
   for the report-shaped fragments, backed by the reports model from day one
   so nothing no-silent-changes produces is ever stringless in between.
7. File lifecycle. Open, save, save-as, recents, refusal surfacing through
   the loader's diagnostic in a toast plus report entry; the default
   directory rule; journal revision exposed as a const query for dirty
   tracking.
8. Settings and sidecar v0. The application store; the sidecar carrying view
   framing only at this stage, written beside the document, versioned; the
   determinism property test lands with the first sidecar byte.
9. Theme singleton and tooltips. The Main.qml hex constants promoted to
   tokens; every control tooltips name plus binding plus description.

Tests: the strategy section's shell additions (workspace lifecycle, dirty,
sidecar round-trip and independence, model diffs); the metadata sweep;
translation tests unchanged and green, since no chord semantics change here;
the existing corpus untouched and green, which is itself the assertion that
rehoming surfaces changed no recording behavior.

By hand: open three documents in tabs and edit all of them; rearrange, lock,
save and reset a layout; drag inside an artificially slowed component and
watch the busy glyph while the pose stays coherent; quit with unsaved work
and get exactly one honest prompt.

Exit: Main.qml is a frame over components; the status mega-string is gone;
every stage 0-8 capability is reachable in the new furniture; flake check
green.

### Stage U1 — vocabulary

Goal: the registry grows every mutating action and read query the spec's
tables name, and the deep panels land as their projections.

Scope, in build order:

1. The ctrl grammar. Binding rows for ctrl+z, ctrl+shift+z, ctrl+d; resolveKey
   resolves explicit ctrl bindings before the swallow default, which stays in
   place for everything unbound; the shell's app-level chords (palette, save,
   open, tab cycling) are documented as the None-fallback tier. The chord
   table goes through the shell translation tests on every platform row the
   suite carries.
2. Styles. Session methods and registry rows for the style family; the
   forking rule (an edit on a style shared beyond the selection forks, an
   edit on an exclusive style mutates, a forked edit reports itself); the
   expression-resistance rule on width and opacity; resolved-appearance and
   style-usage queries. This is the largest single item: it is the first
   family whose Session methods write records no session method has written
   before, and the property tests below pin the forking semantics before any
   toolbar exists.
3. Parameters. Create, set, rename, delete rows over the existing command
   shapes; wouldCycle surfaced at commit; parameter-usage counts; deletion
   confirms with the freeze semantics the model already has.
4. The remainder of the mutating table. layer.rename; layer.activate with the
   active layer as recorded session state and tools stamping it on emitted
   entities; relation.set-value with check-before-drive and the downgrade
   offer on refusal; relation.flip-alternative; relation.retarget-axes as the
   new core entrance to the transform rewrite, with rotate's refusal gates
   (lock, frame-referenced) and the frame joining the selection;
   snap.set-grid and snap.set-construction-attract replacing every raw
   snapPolicy() mutation the shell would otherwise grow.
5. Queries. Constraint list for a selection with values and status flags,
   over a reverse index in core; undo and redo labels; axis references for a
   selection.
6. Surfaces. The inspector (selection, relations, style sections; the
   rectangle size panel rehomes here); the parameters panel; the history
   panel over the labels; the style toolbar; the numeric entry widget
   replacing the status-string field; layers panel rename, activate and the
   badges.

Tests: the conformance sweep over every new action, including under a locked
layer; a corpus script per new mutating action; retarget parity — retargeting
axes without rotating leaves every internal residual exactly zero and matches
rotate-with-retarget's rewrite on the same cluster; the style forking
property over random shared/exclusive configurations; set-value's refusal
path leaves the document byte-identical and the downgrade on offer; the
ctrl chord table; parameter cycle refusal inline at the panel model level.

By hand: recolor a mixed selection and watch the fork report; drive a dozen
stroke widths from one named parameter and scrub it; walk the history panel;
retarget a rectangle's axes to a cluster frame without rotating and then
rotate it; type a width into a dimension from the inspector and refuse one
into a contradiction.

Exit: the spec's action and query tables are fully live, swept and scripted;
every panel binds structure; no raw Session call exists in QML.

### Stage U2 — canvas depth

Goal: the canvas overlays and modes — the legibility features that need core
queries and render drawing rather than panel plumbing.

Scope, in build order:

1. Glyph labels and honest truncation. Value text on valued marks in the
   dimension convention; mnemonic labels under a loose budget, dropped before
   marks as density tightens; the per-anchor fan cap and the ⋯ overflow mark
   placed by the shared layout; visibleGlyphs returns its dropped count; the
   HUD shows N of M; the density preference in settings. The ⋯ pick opens
   the inspector filtered to that anchor.
2. Direction classes and extensions. The core query (declared-parallelism
   closure: parallel edges union, null-reference horizontals one class,
   null-reference verticals one class, named references joining their
   reference); the extension overlay to the viewport edges; hover
   highlighting the class; the HUD count. Segment-only, per the spec's open
   question.
3. Axis visualization. The document frame and cluster frames drawn when the
   selection carries axis relations, construction-tinted; the per-workspace
   all-frames toggle in the sidecar; the inspector's axis section binding the
   retarget action U1 landed.
4. Inspect mode. Document-only rendering — no adorners, no construction, no
   grid, no tint, hidden layers hidden, background applied — with
   navigation-only input and edit keystrokes inert; toolbar button, View
   checkbox, Esc exits. Presentation state, unrecorded, tested inert.
5. Background color. The Adornment field, the sidecar field, the View menu
   and canvas context menu swatch; asserted absent from export bytes.
6. Resize recomposition. The document point at the viewport centre holds
   across a resize; the arithmetic beside the view composition in render,
   headless-tested; the review's suspicion settled deliberately.
7. HUD assembly. dof, zoom, truncation count, class count; corners; click
   through to their panels.

Tests: the raster analytic set from the strategy section; layout agreement
for the overflow mark; direction class determinism and the closure cases;
inspect-mode inertness as a script property (a script recorded across a
toggle is byte-identical to one without it); the resize invariant; sidecar
round-trip of the new fields.

By hand: crowd a vertex with six relations and read it (fan, labels, ⋯,
inspector); turn on extensions over a half-constrained drawing and find the
undeclared near-parallels by eye; work a while in inspect mode and trust it.

Exit: every spec canvas feature is on by default or one toggle away;
truncation is honest everywhere a budget exists.

### Stage U3 — interchange

Goal: files in the UI with the same honesty as the headless paths, and the
developer instrumentation surfaced.

Scope, in build order:

1. Export dialog. Destination, margin, precision; the loss report computed
   from the bake and shown before writing; the write checked end to end with
   a short write surfacing as a failed export; background never exported.
2. Import dialog. A new workspace with the trace; traced and skipped counts
   in the reports panel; the geometry-arrives-free statement in the report.
3. Reports maturity. Click-to-select on entries that name records; entries
   for every producer (a sweep test: each no-silent-changes producer emits
   exactly one entry); the forked-style notice from U1 verified here as part
   of the sweep.
4. Developer surface. Record start/stop to file, replay into a fresh
   workspace with the progress overlay, both in a Developer menu section.
5. CLI file arguments. --export and --import operate on a named document
   rather than the demo; dispatch and exit codes pinned by the CLI tests the
   review noted as an accepted gap, closed here where it is cheap.

Tests: export short-write surfacing; import count parity between dialog and
headless paths on the corpus SVG; the reports sweep; replay overlay smoke
under offscreen QPA; CLI dispatch table.

By hand: export a document with punches and composites, read the loss report,
open the result in an external viewer; import a real-world SVG and read what
was skipped; record a session, replay it in a fresh tab, diff the saves.

Exit: interchange round-trip demos recorded; every report reaches the panel;
no silent success anywhere a file is written.

### Stage U4 — discovery

Goal: the discovery window over the assembled surface. By design this stage
adds no scope; it answers the spec's open questions and freezes what feels
right.

Scope: recorded user-testing sessions into the corpus; layout, density and
toast defaults tuned in settings; the spec's open questions decided — the
strip and numeric entry merge or stay two, the layers panel's region sublist
earns its place or is cut, the rotate preview buttons' home, the forked-style
notice's final form — and UI_SPEC.md amended to record each answer with its
reason. Feel numbers that settle get their corpus entry the same day.

Tests: new corpus entries pinning settled behavior; settings defaults
asserted so a regression in a default is a diff, not a surprise.

Exit: no open question left open in the spec; the production static-scene UI
is declared ready for its first outside users.

## Sequencing rationale

| ordering | reason |
|---|---|
| pump before panels | every readout below assumes honesty under load |
| metadata into U0 | menus arrive in U0 and must be born projections |
| vocabulary before deep panels | a panel bound to a private call is rework; bound to a row it is a rebind |
| snap actions before any settings UI | the first policy checkbox in QML would otherwise be a non-recording mutation |
| overlays after inspector | the overflow mark needs somewhere honest to point |
| interchange after canvas depth | the loss report should describe what the user is looking at, background and all |
| discovery last, scope-free | the same bargain every prior stage struck |

The corpus is the continuity instrument throughout: U0 proves rehoming
changed no behavior by leaving it untouched and green; U1 grows it by one
script per action; U2 and U3 grow it by properties; U4 grows it by settled
feel.

## Risks and de-risking

| risk | probe | contingency |
|---|---|---|
| hand-rolled panel host balloons | U0 midpoint ships fixed zones before drag-reorder lands | KDDockWidgets behind the unchanged panel contract |
| model rebuilds on changed() too slow at scale | bench baseline at U0 exit on reference scenes | granular notification per record kind; the models already isolate consumers |
| ctrl chords misbehave across layouts | the chord table in shell translation tests; scan-code rule already proven | affected chords fall back to the None tier and shell shortcuts |
| style forking surprises users | the fork reports itself from day one; U4 watches | default flips to a role-ambiguity prompt, one predicate change |
| solver gate under two schedulers | audit + two-session worker test when the second scheduler exists | async stays active-workspace-only, which is the U0 posture anyway |
| glyph labels unreadable at density | labels share the mark budget and drop first | mnemonic labels become hover-only; the value labels are the load-bearing half |
| sidecar orphaned by rename or move | none needed — it is droppable by design | the determinism test keeps it unable to matter |
| tabs multiply memory (a session per document) | asan run over the tab stress script in U0 | sessions for hidden tabs drop derived indexes; documents stay |

## Deferred hooks

Unchanged from the spec's parked list, restated with their seams: floating
panels (the panel host contract), rulers and guides (guides are construction
lines the moment rulers exist to drag from), the binding editor (registry
ownership of resolution plus the metadata columns), autosave and crash
recovery (journal replay, a design of its own), per-selection freedom
display (the dof readout's natural growth), localization (the metadata
columns centralize display strings), touch and pen (the translation seam),
theme editing (the token singleton), recognition-as-tags (taxonomy plus
tags, per PRINCIPLES). The timeline inherits the bottom zone, the slot
editors and the never-blocks contract, which is the whole point of having
built this surface the way the spec says to.
