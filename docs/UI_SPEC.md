# UI specification: the production static-scene surface

This document specifies the first production-oriented UI for the single-frame
vector use case. PRINCIPLES.md governs semantics and PLANS.md governs order;
this document governs the surface, and on conflict principles win and this spec
is amended. It is written against the tree at stage 8 completion plus the
stage 0-8 review pass, whose Disposition section named two inheritances for
exactly this work: the pumpAsync integration contract and the deferred ctrl+
binding grammar. Both are in scope here.

The spec is a starting point to be iterated through user testing, not a final
form. What it fixes hard is the machinery that makes that iteration cheap and
safe — which surfaces exist, what each projects, where new vocabulary lands in
the layer stack, and what may never be violated while iterating. What it holds
loosely is arrangement, sizing, iconography and the feel numbers, all of which
are expected to move.

## What exists and what this replaces

The current shell is one QML file of hand-drawn rectangles: the SketchView
canvas, a transient chip strip, a Ctrl+P palette, a read-only layers list, and
a 78px bottom bar whose right-aligned monospace status string concatenates
everything that has no surface of its own — tool state, offers, numeric entry,
deletion counts, structure reports, rectangle panels, broken-tag counts,
crossing-edge notes, dof, solve time and zoom. The comments in Main.qml call it
stand-in furniture, and it is: it exists to keep the model's control planes
warm, and it has done that job.

What is load-bearing underneath it is small and clean, and every piece of it
survives this spec unchanged:

- The event translation contract: `shelltest::translatePointer`, `strokeOf`,
  `engravedDigit` — pure, headless-testable, pinned by tests/shell/. Both
  coordinate spaces filled from one conversion; the engraved digit from the
  native scan code; Control faithfully forwarded and resolved by the registry.
- Everything-through-the-registry dispatch: `resolveKey` → `invokeAction` /
  `type`, and `run()` as the one entrance every clickable surface uses. No QML
  ever calls a Session mutation directly.
- The one-painter rule: one `renderDocument` call into one surface, the same
  path `--selftest` exercises.
- View ownership: pan and zoom owned by the shell, composition arithmetic in
  render, framing latched once and reset only by resetView, edits never
  re-syncing the viewport.
- The recording surfaces: ScriptRecorder wiring, the pendingScript handoff,
  record → replay → record as the identity over every mutating surface.

The production UI is a larger set of projections over the same seams. Nothing
in this spec adds a second path to anything.

## Vocabulary

- **workspace** — one open document with its session, view state, and sidecar
  preferences. Workspace tabs switch between them.
- **layout** — a saved arrangement of panels. Layouts are global to the
  application, not per workspace.
- panel — a dockable surface (layers, inspector, parameters, reports). Panels
  are permanent furniture: spatially stable, dimming what does not apply.
- toolbar — a strip of action buttons projecting a registry category.
- strip — the transient context surface near the work, unchanged in role from
  today: ranked offers, numeric fields, downgrade offers.
- HUD — read-only overlays at the canvas edges: dof, zoom, overflow counts.
- sidecar — the per-document preferences file beside the document, never
  affecting document bytes.
- inspect mode — the WYSIWYG toggle: document-only rendering, navigation-only
  input.
- active layer — the layer new geometry lands on, a recorded session property.

## Surface taxonomy

Three classes of surface, each with different rules, so every later argument
about where something goes has a frame:

Permanent furniture — menu bar, toolbars, panels, status bar, tabs. Spatially
stable: slots never reshuffle with context, inapplicable actions dim and never
vanish, and ordering within a surface is fixed by the registry's table order or
by explicit grouping metadata, never by usage. This is the muscle-memory
surface.

Transient surfaces — the context strip, context menus, popovers, toasts, the
numeric entry. Contextual ranking and content are allowed here and only here,
and every transient surface has a lifetime rule tied to what produced it, the
way the strip's offers already clear on commit, Esc and tool change.

Modal moments — file dialogs and the unsaved-changes prompt. Nothing else. A
diagnostic is never modal: conflicts, degradation, drops and refusals surface
through tints, glyphs, the reports panel and toasts, per the no-silent-changes
policy which is equally a no-interruption policy. The one legitimate modal
beyond file choosing is imminent data loss.

The registry remains the single vocabulary. Menus, toolbars, panels, context
menus, the palette, the strip and the keyboard all project the same table, so
an action reachable one way is reachable every way and a surface can never
offer what the model would refuse. Where this spec adds vocabulary — styles,
parameters, renames — it adds registry actions and Session methods, never
panel-private code paths.

## Window anatomy

```
┌────────────────────────────────────────────────────────────────────┐
│ menu bar                                                           │
├────────────────────────────────────────────────────────────────────┤
│ tabs:  drawing.paro ●   untitled-2                              +  │
├──────┬──────────────────────────────────────────────┬──────────────┤
│ tool │                                              │ layers       │
│ bar  │                                              │              │
│      │                 canvas                       ├──────────────┤
│ con- │                                              │ inspector    │
│ str- │   ┌─────────────────────────┐                │  · selection │
│ aint │   │ strip / numeric entry   │  HUD: dof,zoom │  · relations │
│ bar  │   └─────────────────────────┘                │  · style     │
├──────┴──────────────────────────────────────────────┴──────────────┤
│ status bar: selection ladder · readouts · latest report            │
└────────────────────────────────────────────────────────────────────┘
```

Default layout: tools and the constraints toolbar docked left; layers and the
inspector docked right; parameters, reports and history are closed by default
and open into the right dock. The strip stays where it is today, floating at
the canvas bottom-left near the work. The status bar replaces the 78px bottom
bar at a conventional height.

The canvas is the largest thing on screen at every default. This is a drawing
tool; furniture that squeezes the drawing loses to furniture that collapses.

## Shell architecture

### Projection adapters

The status mega-string is dismantled. SketchView already exposes structured
projections the QML ignores (`solveStatus`, `brokenTags`, `rectangles()`); the
production shell binds structure, not strings. Each panel gets a typed model —
QAbstractListModel subclasses in shell — rebuilt from Session projections on
the `changed()` signal and diffed into place so list selection and scroll
survive a refresh. The single coarse `changed()` cadence is kept for now; per
edit it is one rebuild of a few small models, and granular notification is a
measured optimization, not a design input.

Where each fragment of the status string lands:

| today, in the string | production home |
|---|---|
| tool name and parameters | tool options row under the toolbar |
| numbered offers, auto-commit candidates | the strip (already) |
| numeric field `[123_]` | the numeric entry strip |
| dof, solve time, zoom | status bar readouts, HUD |
| saturation / resisting | canvas tint (already) + status bar note |
| off-screen ripple | canvas edge ping (render) + status note |
| deletion counts, structure reports | toast + reports panel |
| broken tags / regions, crossing note | layers panel badges + reports panel |
| rectangle panels | inspector, size fields |
| script progress | a replay overlay, dev surface |

### Panel host

Panels dock into three zones — left, right, bottom — and are reorderable
within a zone by drag, collapsible to their title, and closable. Every panel is
reachable from View ▸ Panels regardless of state, so no surface can be lost. A
lock toggle on the window freezes all panel geometry: locked panels have no
drag affordance, which is the whole feature. Floating panels are deferred;
the requested capability set — movable, lockable, savable, resettable — does
not require them, and free-floating docking is exactly the kind of dependency
decision (KDDockWidgets versus hand-rolled) that should be made with the
simpler system's shortcomings in hand rather than speculatively.

Layouts are named snapshots of the panel arrangement: zone membership, order,
sizes, collapsed state, lock state. Save layout, apply layout, reset to
default. Stored in application settings, versioned, and the default layout
ships in code so reset works with no settings file at all.

### Settings stores

Three tiers, strictly separated by what they may influence:

- Application settings — theme, layouts, recent files, default directories,
  glyph density preference, binding overrides when they arrive. Stored under
  the platform config location (QStandardPaths::AppConfigLocation: XDG on
  Linux, Library/Preferences on macOS, AppData on Windows). May never influence
  document bytes.
- Workspace sidecar — per-document presentation: view state, background color,
  axis visualization toggles, extension overlay state, grid display preference.
  A small versioned text file beside the document, `name.paro-view`, same
  lexical rules as the document format, droppable without loss of meaning.
  Deleting it costs view preferences and nothing else.
- The document — authoring intent only, exactly as FORMAT.md freezes it. No UI
  state enters it, ever. The additive path for anything that turns out to be
  document-semantic (a document's own default units, say) is a new record kind,
  and that bar is deliberately high.

The determinism consequence is stated as a test: opening a document with or
without its sidecar, editing identically, and saving produces identical
document bytes.

### Workspaces and tabs

One Session per open document, one SketchView per workspace, a tab bar above
the canvas. The registry and all panels project the active workspace's session;
switching tabs swaps the projection source and the view. New tab is a new empty
document; middle-click or the close affordance closes with an unsaved-changes
prompt when dirty (the dirty bit is journal position versus last save). Ctrl+
Tab cycles. Script record and replay bind to one workspace.

Per-workspace state that must survive a tab switch and a session restart rides
the sidecar: view framing, background color, axis and extension toggles, grid
display. Snap-affecting settings are session state set through recorded actions
(below), re-applied from the sidecar on open by invoking those actions before
the user's first gesture.

### Async wiring

The shell finally honors the pumpAsync contract: a timer (frame-adjacent,
~16ms, running only while `asyncBusy()`) calls `pumpAsync()` and repaints on a
true return. It never bumps the epoch. Async solving is enabled with the
existing size threshold policy; the readouts already fold applied async
results. The status bar shows a calm busy glyph while a component is off-thread
— last coherent pose on screen, never a blank, exactly what the async path was
built to buy. This is the first item of implementation, not the last, because
every panel below assumes readouts that stay honest under load.

## Canvas

### Navigation

Unchanged mechanics: wheel zooms anchored on the cursor, middle-drag pans,
framing latches once and belongs to the user. Added surfaces over the same
machinery: zoom in / zoom out / actual size / fit as View menu items and HUD
buttons, all through the existing ViewState composition. resetView gets its
menu row and binding.

Window resize gets the deliberate answer the review's suspicion list asked
for: the document point at the viewport centre stays at the viewport centre
across a resize. The recomposition lives beside the rest of the view
arithmetic in render, where a headless test can hold it.

### Grid

The drawn grid remains the snap policy's grid handed down by the shell — one
number, one promise. The View menu carries grid visibility (sidecar,
presentation-only) separately from grid snapping (session, recorded); the two
share the step so the drawn grid can never lie about where a click lands.
Changing the step is a recorded action (below).

### Background color

The canvas background becomes a per-workspace setting, defaulting to the
application theme's background. It exists to iterate on palettes and contrast
against discrete scene shapes, so it is deliberately unexported: the bake and
the SVG writer never see it, which is already true and stays true. Plumbing
follows the grid precedent exactly — the shell hands the color down in the
Adornment, render draws no default guess of its own beyond the compiled-in
fallback. Sidecar-persisted. A small swatch control lives in the View menu and
the canvas context menu.

### Inspect mode

One button on the toolbar and a View menu checkbox, one keystroke. Inspect
mode renders the document as output: no vertices, no glyphs, no handles, no
dimension text, no construction geometry, no grid, no selection tint — hidden
layers stay hidden, the background color applies, and what is on screen is
what an export would mean. Input is navigation only; pointer edits and edit
keystrokes are inert (not queued, not reinterpreted), and Esc or the toggle
exits. Because no edit can occur inside it, it is presentation state, not a
recorded action, and scripts are unaffected.

This is deliberately a hard mode rather than a per-overlay checklist: its
value is the single gesture between "editing scaffolding" and "the actual
graphic", and a mode that hid only some adornment would answer neither
question. Per-overlay toggles (glyphs off but handles on) are View menu items
that exist independently of it.

### HUD

Read-only, canvas corners, never interactive beyond a click-through to their
panel: dof readout (calm, tinted only when nonzero, "unsolved" only when a
component failed), zoom percentage, the glyph overflow count when truncation
is active ("34 of 61 relations shown"), the direction-class count while the
extension overlay is on. The saturation and ripple indications stay on the
canvas itself where they already live.

## Toolbars

### Tools

Select, line, circle, arc, rectangle — the five tool actions, iconized, with
the active tool held visibly down. Below them, a tool options row shows the
active tool's live parameters (the same `ToolParameter` data the status string
shows today) as read-only fields that the numeric entry fills; clicking one
opens the numeric entry on that field, which is precisely what Tab reaches by
keyboard. Esc discipline is unchanged and the toolbar reflects it: first Esc
ends the placement, second returns to select, and the pressed state tracks the
session, never a local toggle.

### Constraints toolbar

The CAD-surface projection of the 66 generated imposition rows, grouped by
family in fixed order:

| group | kinds |
|---|---|
| placement | coincident, point-on-line, point-on-circle, midpoint |
| direction | horizontal, vertical, parallel, perpendicular, angle, equal-angle |
| size | distance, point-line-distance, radius, equal-radius, equal-length, length-ratio, length-difference |
| symmetry | symmetric-horizontal, symmetric-vertical, symmetric-about-line |
| curve | tangent |
| anchor | pin |

Each button's enablement is `canImpose` for its kind over the live selection —
the same predicate the strip and palette read, so the three surfaces can never
disagree. Hovering an enabled button ghosts the imposition through the
existing preview path with its verdict line; a kind whose reading is ambiguous
(`orderSensitive` or grouped operands) opens a popover listing the named
assignments exactly as the strip names them, because role ambiguity resolves
in the surface, not in prose.

Click imposes at driving strength. Each button carries the other two strengths
— reference and measure-once — in a press-and-hold / right-click flyout, so the
three-strengths grammar is visible on the surface rather than buried in the
palette's 66 names. The flyout order never changes; strength is a property of
the click, never a sticky toolbar mode, because a mode would make the same
click mean different things on different days.

The toolbar also carries toggle-driving and walk-conflicts, dimmed by their
existing predicates.

### Style toolbar

Stroke color, fill color, stroke width, opacity, filled flag — reading the
selection's resolved appearance and writing through the new style actions
(below). Mixed selections show mixed-state controls. Width and opacity are
slots: the toolbar edits constants and scrubs like any numeric field, and when
a slot holds an expression the control shows the expression read-only and
resists direct edits, pointing at the parameters panel — the same rule the
rectangle handle already follows, because a value authored elsewhere deserves
resistance, not silent flattening.

Style semantics on the surface: an entity or region either references a shared
StyleRecord or none. Editing appearance on a selection whose style is shared
with things outside the selection forks — the selection gets its own style
record with the edit applied — because direct manipulation must affect what
the user is holding and nothing else. Editing the shared record itself is done
by name in the inspector's style section, where "used by N" is visible before
the edit lands. Construction geometry shows no style controls; its role is
what it is rather than how it looks.

## Panels

### Layers

The expressive successor of the read-only list. One row per layer in order,
base layer included and unnamed-but-renameable; per-row inline toggles for
visible and locked; drag to reorder (projecting raise/lower); double-click to
rename; a row highlight for the active layer and click-to-activate. A new-layer
button and a delete affordance that projects the model's refuse-while-
referenced behavior honestly: delete on a populated layer offers "move contents
to base and remove" as the two-step the model requires, never a silent cascade.

Rows carry badges, not colors alone: a lock badge, a hidden badge, a count of
entities, and degradation badges when the layer holds broken regions or tags —
click-through to the reports panel. Right-click opens the layer context menu
projecting the full `layer.*` family plus move-selection-to-layer, dimmed by
applicability.

Groups and tags appear in this panel as a second section rather than a tree
mixed into layers, because in this model layers are not a scene graph — they
are organization, constraints cross them freely, and a false hierarchy would
teach a false model. Groups list their member counts with select-on-click,
group/dissolve buttons; tags list kind and wholeness with select and dissolve.
Regions within a layer are shown as an expandable sublist ordered by z,
projecting raise/lower/punch and showing the broken diagnostic state where
regionState reports it.

### Inspector

The selection-facing panel, three stacked sections that appear when they
apply:

Selection — the signature line ("2 segments, 1 point"), the ladder position as
a breadcrumb (shape ▸ edges ▸ points; click ascends, mirroring Esc), and the
selection's layer/group membership with move-to affordances.

Relations — the full constraint list for the selection, unbudgeted and
untruncated, which is the recall counterpart of the canvas glyph budget: the
canvas shows what fits, the inspector shows everything. One row per
constraint: kind glyph and name, operand summary, value (driving plain,
reference bracketed, live), driving/reference state, and status flags —
conflicting, redundant-flagged, frozen-by-lock. Row actions: select (which
also selects the glyph on canvas), toggle driving, edit value (opens numeric
entry on the slot; checked before it drives, downgrade offer on refusal —
the panel imposition rule the rectangle panel already established), flip a
tangency's end, delete. With nothing selected the section offers the document-
wide list, filterable by kind, so "find every distance in this drawing" is one
click and no walk.

Style — the appearance of the selection as in the style toolbar, plus the
named-styles list: create style from selection, apply, rename; "used by N"
before any shared edit.

For a selection carrying a rectangle tag, the inspector shows the size panel:
width and height fields driving through the existing checked tag.set-width /
set-height path, with the dimensioned/undimensioned state of each side legible.

### Parameters

Named document parameters: list, add, rename, edit value — where the value
field is the full slot editor: constant, arithmetic, references to other
parameters, with wouldCycle refusal surfaced inline at commit, not after. Each
row shows "referenced by N slots"; deleting one states the model's actual
semantics in its confirmation — referring slots freeze to their evaluated
values, nothing moves, only provenance is lost. This panel is the payoff
surface of the slot thread: a parameter driving twenty widths is edited here,
once.

### Reports

The append-only session log of everything no-silent-changes generates:
deletion counts, copy drop counts, structure reports (moved, retargeted,
rescaled, straddling, dropped relations/regions/tags, with the frame named),
mirror and distribute reports, import trace counts, export loss reports,
hidden-influence notices, refused impositions with their verdicts. Each entry
timestamped and, where it names records, click-to-select. The latest entry
also flashes as a toast near the canvas for a few seconds — the toast is the
notice, the panel is the memory. Nothing here is modal and nothing here is
required reading to keep working.

### History

The undo journal as a list, labels visible, click to walk to a position
(repeated undo/redo through the ordinary path, so branch fidelity holds).
Requires the journal's step labels exposed through Session (new query, below).

## Constraint presence on the canvas

Glyph marks stay the primary presence — every relation reachable from the
geometry it binds — with three additions:

Labels. A valued mark (distance, radius, angle, ratio, difference) carries its
value text in the same convention dimension text already fixed: driving plain,
reference bracketed, display rounding never entering storage. An unvalued mark
carries a short mnemonic label when the density budget is loose and drops the
text before it drops the mark as density tightens. Labels live in the same
budget as marks, because two budgets over one overlay is how overlays become
unreadable in two different ways at once.

Per-anchor overflow. The fan-out around a shared anchor caps at a small policy
count; beyond it the anchor draws its fanned marks plus one ⋯ mark. The ⋯ is
itself pickable and opens the inspector's relations section filtered to that
anchor's operand — detailed inspection is one click from the crowd. This is a
new GlyphPolicy number and a fan-limit in the shared layout, so render and hit
testing keep agreeing about where marks are.

Honest truncation. visibleGlyphs returns its dropped count instead of
truncating silently; the HUD shows "N of M relations shown" whenever N < M,
and the glyph density preference (application settings, display-only, safe
because scripts never record feel policy) lets the user trade density against
legibility.

## Reference axes

The model already has the whole story — horizontal and vertical as axis-
referenced parallelism with a nullable reference, cluster frames as pinned
construction geometry, retarget as a transform answer. The UI makes it
graphical:

Visualization. When the selection carries axis-referenced relations, the
canvas draws the referenced frame: the document frame as subtle full-viewport
axes through the origin, a cluster frame as its two axes through the frame
segment, tinted as construction. A per-workspace toggle (sidecar) shows all
frames all the time for orientation-heavy work.

Control. The inspector's relations section shows, for the axis family, what
the reference is — "document frame" or the frame's name — and offers retarget:
to a new frame built from the selection, to an existing frame, or back to the
document frame. This is a new recorded action (below) applying the same
retarget rewrite rotate already performs, without requiring a rotation to
reach it. Preview before commit, straddle and refusal semantics identical to
the transform path, frame joins the selection as it does under rotate.

The rotate and scale flows keep their question-with-preview form and gain the
graphical surface: invoking rotate from menu or palette opens the numeric
entry for degrees with the axis answer as a labeled pair of preview buttons,
which is the same two-answers-with-preview PRINCIPLES fixed, now visible.

## Line extensions and direction classes

A View toggle draws every segment's carrier line extended thin and dim to the
viewport edges. While it is on:

- The HUD shows the distinct direction count, where directions are counted by
  declared parallelism: the closure of segments over parallel constraints,
  null-reference horizontals as one class, null-reference verticals as one
  class, named references joining their reference's class. Two segments that
  merely look parallel count twice — that discrepancy is the feature, because
  it surfaces undeclared structure, which is exactly the thesis's diagnostic:
  the count reads the declarations, not the pixels.
- Hovering a segment highlights every extension in its class, so "what else is
  parallel to this by declaration" is a hover, not an audit.

The class computation is a core query (union-find over the constraint graph's
direction relations, deterministic order), interact exposes it beside the
other selection queries, render draws it. Per-workspace toggle, sidecar-
persisted, presentation-only.

## Selection and editing surfaces

The pointer grammar is unchanged and this spec adds only legibility:

- The status bar carries the ladder breadcrumb and the signature line.
- Context menu on the canvas: a transient projection over the current
  selection — the strip's offers, then the applicable structure and region
  actions, grouped in fixed order, dimmed not hidden. Right-click selects
  under the cursor first if the press landed on something unselected, exactly
  as a left press would.
- The numeric entry becomes a real widget in the strip's position: visible
  field, name, unit echo, Tab cycling across fields, Enter and Shift+Enter
  exactly as today. It is presentation over the same NumericEntry state — the
  keystroke grammar does not change, the field just stops living inside a
  status string.
- Parameterized actions invoked from name-only surfaces (menus, palette rows
  without preset readings) open the numeric entry pending the action rather
  than executing with a remembered value, preserving the rule that an action
  whose value is a required number cannot be invoked from a surface that knows
  only its name. The palette keeps its preset readings (quarter turns,
  doubling) as today.

## Files

### Lifecycle

New, open, save, save-as, close, recent files. Save writes through persist to
the workspace's path; fast save with no path yet goes straight to save-as. The
default directory for both save-as and open is the platform documents location
plus Paroculus (QStandardPaths::DocumentsLocation — xdg-user-dirs on Linux,
~/Documents on macOS, the Documents known folder on Windows), created on first
save, remembered per-machine in application settings once the user chooses
anywhere else. Autosave is deferred deliberately: the journal makes in-session
loss a non-event, crash recovery is a real feature that deserves its own
design (journal replay, not file shadowing), and a half-designed autosave that
rewrites user files on a timer is worse than none.

Open validates through the loader's existing parse-or-refuse discipline and
surfaces a refusal with the loader's line diagnostic in a toast plus reports
entry — never a half-loaded document. A greater-versioned file refuses whole
with wording that says newer, not corrupt.

### Export and import

Export SVG is a dialog: destination, margin and precision (the existing
SvgOptions), and a live loss report computed from the bake before writing —
what flattens, what drops, the counts the bake already carries — so the lossy
step is consented to, not discovered. The write checks the stream end to end;
a short write is a failed export, loudly. Background color is not exported.

Import SVG is a dialog opening a new workspace with the trace, reporting
traced and skipped counts in the reports panel. Geometry arrives free and
unconstrained, stated in the report, because a user who does not know import
is a trace will blame the constraints they never had.

Record and replay stay first-class but move to a Developer menu section:
start/stop recording to a file, replay a file into a fresh workspace with the
progress overlay. They are the instrument this whole surface is tested with
and hiding them entirely would be self-sabotage.

## Keyboard and input

The registry stays the sole resolver of keyboard meaning. The binding grammar
grows ctrl+ rows (the review's finding 14 deferral, now due): ctrl+z undo,
ctrl+shift+z redo, ctrl+d duplicate, and room for the conventional file chords.
Session-affecting chords resolve in the registry; application chords that
touch no session (palette, save, open, tab cycling) stay shell-handled behind
the registry's explicit None, which is today's Ctrl+P mechanism made policy:
resolveKey first, shell shortcuts only for what it declines. The engraved-
digit rule, the Control-swallow default for unbound chords, and every existing
single-letter binding are unchanged.

Menus display their actions' bindings, tooltips everywhere show name plus
binding, and the palette remains the everything-surface with its subsequence
filter. Action rows gain description and category metadata — two more data
columns on the one table, consumed by menus for grouping and by the palette
for a richer row, never a second list. A user binding editor is deferred; the
metadata and the registry's ownership of resolution are what make it cheap
later.

## New interact vocabulary

Every new capability above that mutates the document lands as Session methods
plus registry actions, so every surface stays a projection and every edit
stays recorded. The additions, with their applicability:

| action | parameters | applicable when |
|---|---|---|
| style.set-stroke | color | selection has styleable entities |
| style.set-fill | color | selection has styleable regions/entities |
| style.set-stroke-width | value | as above, slot not expression-driven |
| style.set-opacity | value | as above |
| style.set-filled | flag | selection has regions |
| style.create | from selection | selection styleable |
| style.apply | style id | named style exists, selection styleable |
| style.rename | style id, name | style exists |
| parameter.create | name, value | always |
| parameter.set | id, value/expression | parameter exists, no cycle |
| parameter.rename | id, name | parameter exists |
| parameter.delete | id | parameter exists (freeze semantics) |
| layer.rename | layer, name | layer exists |
| layer.activate | layer | layer exists, unlocked |
| relation.set-value | constraint, value | valued constraint selected, checked-before-drive |
| relation.flip-alternative | constraint | kind has alternatives |
| relation.retarget-axes | frame / document | selection carries axis relations, no lock, no frame-referenced kind |
| snap.set-grid | step, enabled | always |
| snap.set-construction-attract | flag | always |

The snap rows exist because the policy fields are edit-affecting: a toggle
mutated through the raw policy reference is a non-recording surface, and a
script replayed under different snapping is a different drawing. Grid
visibility, by contrast, is presentation and stays out.

New queries (no recording, read-only): constraints-for-selection with values
and status flags (the inspector's list, with the entity→constraint reverse
index selection.h already notes it wants), undo/redo step labels, direction
classes, axis references for a selection, style-usage and parameter-usage
counts, and the glyph overflow count. Each sits in interact or core by the
existing rule — shared by two layers means core, session-shaped means
interact.

Everything added enters the existing conformance machinery: every action
headlessly invocable, applicability sweeps extended to cover the new
predicates (including under lock, closing the review's untested
applicable-equals-runnable-under-lock gap), record → replay → record over
every new mutating action.

## New core and render work

| work | layer | note |
|---|---|---|
| GlyphMark label text + per-anchor fan limit + overflow count | core (data, layout) + interact (budget) + render (drawing) | one layout, two readers, as today |
| direction-class query | core | beside topology; deterministic order |
| constraint reverse index | core | the index selection.h anticipates |
| retarget-axes step reusing transform's rewrite | core | same machinery, new entrance |
| background color handed down | render | Adornment field, grid precedent |
| line-extension overlay | render | pure drawing over the class query |
| resize recomposition (centre-stable) | render | beside the view composition |
| journal step labels exposed | core → interact | labels already exist in the journal |

Nothing here touches solve, and nothing changes the document format. The one
candidate for a format change — a document-semantic presentation record —
is deliberately not taken now; every candidate field found a better home in
the sidecar or in styles.

## Async, performance, responsiveness

The UI never blocks on a solve: synchronous under budget, async above it, the
pump on its timer, last coherent pose always on screen. Panel rebuilds are
bounded by document size, not by solve time, and happen on the UI thread from
immutable projections. The no-mutexes rule is untouched — panels read
projections, workers never see the UI.

Feel numbers new to this spec (fan limit, toast duration, HUD placement, dock
sizes) join policies and settings the way existing feel numbers do: replaceable
values, corpus-visible where they affect edits, plain preferences where they
are display-only.

## Accessibility, DPI, theming

Logical pixels everywhere above render's deviceScale, as today; panel and font
sizing follow the platform scale factor through Qt. The theme is a token set
(the current inline hex constants of Main.qml promoted to a single QML theme
singleton), dark by default, with the render-side geometry palette staying
render's own — canvas tints are semantics (selection, resistance, roles), not
theme, and recoloring them is a feel decision with corpus consequences, not a
skin. Tooltips on every control: title, binding, one-line description from the
action metadata. Full keyboard reachability holds by construction because
every action is registry-projected. Localization is explicitly deferred; the
action metadata columns are where display strings will centralize when it
arrives.

## Animation-forward constraints

This UI's shape is what the animation scope will inherit, so four commitments
are made now, cheaply, to avoid prohibitive rework later:

- The bottom dock zone exists from day one and stays uncluttered by default.
  It is where a timeline lands, full-width under the canvas, without evicting
  established furniture.
- Every value cell in every panel is a slot editor, not a number editor —
  constant today, expression today, track later. No panel may assume a value
  is a plain double, which the style toolbar's expression-resistance rule
  already enforces in miniature.
- Workspaces stay one-document-one-session. A scene with time is still one
  document; tabs never become the timeline's axis.
- Nothing modal, nothing blocking, and readouts fold async by construction —
  a scrubbing timeline is a storm of solves, and every surface this spec adds
  already lives under the never-blocks contract.

## Phasing

Ordered so every phase ships a usable improvement and no phase builds on
unlanded vocabulary. Registry and interact additions precede the surfaces
that project them, mirroring the plan's registry-before-surfaces rule.

- U0, scaffolding: pumpAsync timer, panel host and layouts, tabs and
  multi-workspace, menu bar and toolbars over the existing table, typed panel
  models, status string dismantled, settings stores, file lifecycle with
  save/open dialogs. No interact changes; everything projects what exists.
- U1, vocabulary: the new-actions table above, ctrl grammar, queries (labels,
  constraint list, usage counts), conformance sweeps extended. The inspector,
  parameters panel, style toolbar and history panel land on top.
- U2, canvas depth: glyph labels and overflow, axis visualization and
  retarget surface, direction classes and extensions, inspect mode,
  background color, HUD.
- U3, interchange polish: export dialog with pre-write loss report, import
  workspace flow, reports panel maturity, developer record/replay surface.
- U4, discovery window: user testing over the assembled surface, layout and
  density iteration, feel adjustments frozen into settings defaults and
  corpus entries per the established loop.

Each phase exits green: `nix flake check`, the corpus, the shell translation
tests extended over every new chord and control, and the record → replay →
record identity over every new mutating surface.

## Parked for discovery

Named so their absence is legible, per the principles' own convention: floating
panels and a docking dependency, rulers and guides-from-rulers, a binding
editor, autosave and crash recovery, per-selection freedom display, snapping
distance readouts during drag, marquee modes (crossing versus containing),
canvas rotation, touch and pen input, localization, theme editing, and
recognition of hand-built structures as tags. Each has a stated seam to hang
from; none blocks the phases above.

## Open questions

Carried explicitly rather than decided by accident:

- Whether the strip and the numeric entry merge into one bottom-of-canvas
  surface or stay two; user testing on U0 decides.
- Whether direction classes should also count arcs and circles by tangency
  families or stay segment-only; segment-only ships first.
- Whether the layers panel's region sublist earns its place or z-order
  manipulation stays canvas-and-context-menu; measured in U2's window.
- Where exactly the axis-question preview buttons live when rotate is invoked
  from the toolbar versus the palette; both must show the same two answers.
- Whether style forking on shared-style edits needs an undo-adjacent notice
  ("forked from style X") in the reports panel; leaning yes, costs one line.
