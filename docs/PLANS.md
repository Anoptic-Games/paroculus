# Implementation plan

This document turns PRINCIPLES.md into modules, an architecture, and a
sequence of bounded stages with the tests each stage must carry. PRINCIPLES.md
governs semantics; this document governs order. On conflict, principles win
and the plan is amended. Feel-tuning items stay out of stage scopes by design:
each interactive stage ends with a discovery window whose findings land as
policy adjustments plus frozen invariants, not as new scope.

Ground rules for every stage: the application builds and runs at stage exit
(`nix build`, `nix flake check`, `nix run .#dev` all green); the cumulative
test corpus from prior stages keeps passing; each stage ends in a reviewable
state for manual testing, after which commits are drafted by hand. No stage
begins before its predecessor's exit criteria are met, except the explicitly
parallel renderer track.

## Architecture

### Module map

Five layers, each crossable downward only. The seams from PRINCIPLES
(document / solve / raster / toolkit) become CMake targets, so a forbidden
include is a build error, not a convention.

```
app        main, selftest, script playback
shell      Qt: SketchView, input translation, QML surfaces        [Qt]
interact   selection, hit, snap, tools, registry, numeric, policies
render     tessellation, compositor, adorners, view painting      [Skia]
solve      slvs translation, contexts, seeds, diagnosis           [slvs.h]
core       ids, taxonomy, slots, units, document, topology, geom  [Eigen]
```

Dependencies: shell → interact + render; interact → core + solve;
render → core; solve → core; core → Eigen only. The interact layer is
deliberately toolkit-free and raster-free: it consumes abstract input events
(positions in both spaces, buttons, modifiers, keys) and emits document
commands plus transient presentation state (rubber bands, ghost candidates,
strip offers). That is what makes gesture scripts runnable headlessly in CI,
which the whole feel-freezing strategy depends on. Shell-owned facilities the
tools need (clipboard, cursor shape) are reached through small port
interfaces implemented by shell, keeping the dependency arrow pointing down.

### Targets and layout

```
paroculus-core      src/core      no Qt, no Skia, no slvs
paroculus-solve     src/solve     links paroculus-solver PRIVATE; slvs.h
                                  never reaches its public headers
paroculus-render    src/render    Skia; no Qt
paroculus-interact  src/interact  core + solve; no Qt, no Skia
paroculus           src/shell,    Qt shell and entry point
                    src/app
paroculus-tests     tests/        doctest runner over all layers below shell
```

`paroculus-solver` (the vendored SolveSpace TUs) already exists and keeps its
role; `paroculus-solve` is our translation layer above it. Linking the solver
privately is what upgrades the header seam from convention to build error.

```
src/
  core/       ids, taxonomy, slots, units, document, commands, undo,
              topology, geom
  solve/      translate, context, seeds, diagnose, (later) scheduler
  interact/   selection, hit, snap, tools/, registry, numeric, policies
  render/     view, tess, regions, adorners, fonts
  shell/      sketchview, inputmap, qml/
  app/        main, selftest, scriptplay
tests/
  unit/  property/  semantics/  gestures/  raster/  bench/  corpus/
```

### Module inventory

| Module | Owns | Must not know about |
|---|---|---|
| core/ids | typed persistent IDs, allocation, ID→index maps | everything else |
| core/taxonomy | single-source tables: entity kinds (fields, param counts), constraint kinds (operand signatures, value arity, solver mapping, invariance class), snap candidate kinds, applicability data | Qt, Skia, slvs |
| core/slots | value cells, named parameters, constant + arithmetic + reference evaluation, acyclicity | solver |
| core/units | unit parse/format, the single conversion boundary | everything above core |
| core/document | records for entities/constraints/regions/roles/tags/styles/layers/groups, edit commands, undo journal over exactly invertible commands, snapshots | Qt, Skia, slvs |
| core/topology | coincidence graph, component partition rebuilt on demand, cycle queries, connected-run queries | solver handles |
| core/geom | measurement math (distance, angle, length, intersection), capture-current-value, the doc↔screen transform type | ownership of view state |
| solve/* | component → `Slvs_System` translation (arena-backed), invocation, readback, seed store, dragged sets, speculative contexts as value types, failure mapping, generation tokens | Qt, Skia, document mutation |
| interact/* | selection model + signatures, spatial index + hit policy, snap candidate generation + ranking, tool state machines, action registry + dispatch, numeric entry sessions, replaceable feel policies | Qt, Skia |
| render/* | solved-geometry tessellation, region fill and alpha-overwrite compositing, screen-space adorners, dimension text (bundled typeface), view framing and the pan/zoom composition over it, painting into caller surfaces | Qt, document mutation |
| shell/* | QQuickItem hosting, QEvent → interact events, QML surfaces projected from the registry, view state ownership | slvs, Skia types beyond the paint interface |
| app/* | entry, --selftest, --script playback of corpus scenarios | — |

Feel policies (hit priority, snap ranking, glyph budget, locality weighting,
async thresholds) are plain replaceable functions in interact/policies with
corpus tests pinning current behavior; changing a policy means updating the
corpus deliberately, never silently.

### Threading and memory

Single writer: the document mutates only on the UI thread. Solves run
synchronously in-frame while under budget; the asynchronous path (stage 8)
moves over-budget components to a worker pool fed by immutable component
snapshots through bounded lock-free SPSC rings, results tagged with
generation tokens and stale ones dropped. Qt's internal locking is outside
the no-locks rule; our code holds none on the interaction path. Every solve
context owns a mimalloc heap (`mi_heap_new`/`mi_heap_destroy`) for
translation scratch, and the vendored solver already arenas its temporaries
through the same mechanism. Document storage is per-kind contiguous tables
with free lists behind stable IDs; snapshots copy only the param spans of the
affected component, which is what keeps speculative contexts cheap enough to
run per hover.

## Test strategy

The suite validates core math and logic in layers, cheapest first. All of it
below the shell runs headless; `nix flake check` runs everything the sandbox
can (unit, property, semantics, gestures, raster analytic, selftest under
offscreen QPA), and `tests/bench` runs on demand with recorded baselines.

- Unit tests: doctest (header-only, packaged in nixpkgs, test-only
  dependency). Hand-rolled deterministic generators (seeded PRNG) for
  property tests; a shrinking framework is adopted later only if failure
  triage demands it.
- Constraint semantics suite: the heart of math validation. For every
  catalogue entry, a minimal system seeded off-constraint, solved, and
  verified by geometric residual — never by trusting the solver's status —
  extending the existing selftest pattern. Each entry also carries an
  infeasible variant asserting `SLVS_RESULT_INCONSISTENT` plus the expected
  `failed` set, and a redundancy variant asserting creation-time detection.
- Property tests: apply+undo returns the document to byte-identical
  serialization; serialize(load(serialize(d))) == serialize(d); incremental
  topology equals from-scratch recomputation after every step of random edit
  scripts (a marked partition, not an incrementally maintained one — see the
  stage 4 amendment); imposition is movement-free within tolerance; copied
  subgraphs are kind-preserving isomorphic with fresh disjoint IDs.
- Conformance sweeps, generated from the taxonomy: every constraint kind ×
  every operand signature — the applicability predicate must agree with the
  outcome of actually attempting it; every record kind serializes and
  round-trips; every registered action is headlessly invocable. These sweeps
  are what keep the taxonomy's five projections from drifting.
- Gesture corpus: synthetic input scripts driven through interact with no
  toolkit, asserting feel invariants — drag locality (params outside the
  dragged component bit-unchanged), saturation (constraints never violated
  beyond tolerance; cursor gap opens only when infeasible), release-commit
  (seeds equal rendered state), WYSIWYG inference (preview candidate set
  equals committed set), inference precision/recall against expected
  constraint sets. Corpus entries start as C++ builders; a recorder and text
  format arrive when feel iteration begins in earnest.
- Raster tests: analytic pixel sampling first (points known inside/outside a
  fill, punched pixels transparent, adorner anchoring), a small set of
  tolerance-based golden images second — Skia is pinned by the flake, which
  keeps goldens meaningful.
- Determinism tests: same build, same machine — repeated solves bitwise
  identical; document rebuilt in permuted insertion order solves within
  tolerance; serialization byte-stable across runs.
- Benchmarks as gates: warm/cold solve times across component sizes
  (8..1024 params, geometric), translation overhead, partition maintenance
  per edit, frame time on reference scenes. Baselines recorded per machine;
  gates compare ratios against baseline with generous margins, absolute
  ceilings only as sanity rails.

Tolerance policy: document units are nominally millimetres with fixtures at
unit scale using absolute 1e-6 (matching the current selftest); large-range
property tests use relative tolerance; screen-space epsilons are pixel
quantities and never appear in core math tests.

The feel-freezing loop: when a discovery window lands on behavior that feels
right, the session is recorded into the corpus and its distinguishing
measurement becomes an invariant. Feel is subjective once; the corpus keeps
it objective afterwards.

## Stage sequence

| Stage | Name | Delivers | Size |
|---|---|---|---|
| 0 | Seams and harness | target split, test runner, CI wiring | S |
| 1 | Document core | records, commands, undo, topology, persist v0 | L |
| 2 | Solve layer | translation, seeds, contexts, semantics suite | L |
| 3 | Interactive loop | view, hit, selection, drag, gesture harness | L |
| 4 | Drawing and inference | tools, snap-as-candidates, numeric twin | L |
| 5 | Action surface | registry projections, imposition, regions v0 | L |
| 6 | Composition | fills, alpha overwrite, layers, degradation | M |
| 7 | Structure operations | transforms, copy, compounds, tags | M |
| 8 | Durability and scale | format freeze, async solving, export | M |
| R | Renderer track | GPU path behind the paint interface | M, parallel |

Stages 4 and 5 are the largest and each has a designated midpoint usable as a
checkpoint: stage 4 after the line tool with coincidence/horizontal/vertical
inference; stage 5 after registry projections plus imposition of distance,
parallel, and equal.

Stages 0 through 4 are complete and were reviewed as a block; docs/REVIEW.md
holds the findings and all of them are closed. Where a finding changed what a
stage does rather than merely what its code says, the stage carries an amendment
paragraph — this document governs order, so a plan the work diverged from is
worse than no plan. Work moved both ways: two format changes came forward into
stage 4, and typing during a drag of existing geometry went back to stage 5.

### Stage 0 — seams and harness

Goal: make the architecture enforceable and the test loop real before any
feature code exists.

Scope: split `sketch.{h,cpp}` along the seams (demo document construction,
solve invocation, raster) into the target layout above; create the CMake
targets with their link boundaries; add doctest and the `paroculus-tests`
runner; extend the flake's `tests` output to run ctest (selftest included);
keep `--selftest` byte-for-byte in behavior.

Tests: the existing selftest, relocated and green; one trivial doctest per
target proving the harness compiles against each layer; a link-boundary
canary (a deliberately illegal include in a scratch TU is compile-checked to
fail — kept as documentation of the mechanism, then removed).

By hand: `nix run .#dev` shows the demo unchanged.

Exit: all outputs green; `sketch.{h,cpp}` gone; boundaries enforced by the
build.

### Stage 1 — document core

Goal: the declaration layer exists, is editable through commands, undoes
faithfully, and round-trips to disk — before any solver integration, so the
model's semantics are pinned by tests that owe nothing to solver behavior.

Scope: typed IDs and allocation; taxonomy tables as a single source (entity
kinds: point, segment, circle, arc; the full v1 constraint catalogue with
operand signatures, value arity, invariance class, solver mapping id; snap
candidate kinds declared even though the snap engine comes later) with its
projections generated from one list; slots with constants, named parameters,
arithmetic, acyclicity enforcement; units with the single conversion
boundary; document record tables and the command set (add/remove entity,
add/remove constraint, set slot, set seeds, set role, region and tag
commands, style and layer commands) with inverses; the undo journal carrying
seed spans from day one, even while seeds are always empty; the coincidence
graph with incremental component partition and cycle query; persist v0 —
text format, version header, stable ID-ordered output, unknown-record
preservation.

Design decisions made in-stage: taxonomy single-source mechanism (x-macro or
constexpr table — requirement is one list, five projections); command
granularity (one user gesture, composites bundle inference later); the
canonical signature form for selections.

Tests: ID stability and non-reuse; taxonomy validation (constraint records
with wrong operand kinds rejected); slot evaluation incl. cycle rejection;
apply+undo byte-identity over random command scripts (property, all
prefixes); redo-all equals final state; incremental partition and cycle sets
equal from-scratch recomputation after every step of random edit scripts
(property); persist round-trip byte-identity; unknown-kind survival;
serialization byte-stability across runs.

By hand: nothing new visible; the demo now constructs its content as a real
document.

Exit: property suites green over generous iteration counts; format versioned
from first write.

### Stage 2 — solve layer

Goal: documents solve; the math of every catalogue constraint is validated;
seeds, branches, and speculative contexts behave as PRINCIPLES demands. This
stage is the heart of core-math validation.

Scope: component → `Slvs_System` translation with ID-ordered determinism and
an arena per context; readback into solved state; the seed store and
seed-commit flow; dragged-set support (`Slvs_System.dragged`); pin as
`SLVS_C_WHERE_DRAGGED`; speculative contexts — fork a component's params,
solve the copy, discard — as cheap value types; failure mapping (`failed`
set → constraint IDs); DOF readback; creation-time speculative checks
(consistency and redundancy) as a library call the action layer will use;
the demo rebuilt on the real pipeline: document → solve → render.

Tests: the constraint semantics suite over the entire v1 catalogue (residual
verification per entry, off-constraint seeds, infeasible variant with
expected failed set, redundancy variant); the two-branch canonical fixture —
a point at fixed distances from two pinned points — asserting seed proximity
selects the branch, warm-started sweeps never flip it (sign invariant across
a scripted value sweep), and cold re-solve from stored seeds reproduces the
recorded branch; component isolation (edits in one component leave others'
params bit-identical); determinism (repeat-solve bitwise; permuted build
within tolerance); translation correctness sweep generated from the taxonomy
(every constraint kind maps, solves, reads back); bench baselines recorded
(warm/cold × component size, translation overhead) with provisional budget:
warm solve of a 256-param component well inside a frame.

By hand: demo behaves identically; a debug readout shows dof and solve time.

Exit: semantics suite exhaustive over the catalogue; branch fixture green;
baselines recorded and committed alongside the bench harness.

### Stage 3 — interactive loop

Goal: the first real interaction: select things, drag them under
constraints, feel resistance, undo it — with the gesture harness proving the
feel invariants headlessly.

Scope: view state (pan/zoom) owned by shell, transform type in core/geom
used by both sides; rendering of document geometry with minimal styling;
spatial index over solved geometry; hit-testing with the priority policy as
a replaceable function; selection model — click, marquee, additive
modifiers, double-click depth descent along coincidence runs, Esc ascent,
signature computation; drag-as-solve with warm starts, release-commits-seeds
as a command, no spring-back; saturation with attribution (soft dragged-set
solve gives the saturated pose; a throttled leave-one-out counterfactual
names the resisting set when the cursor gap opens); off-screen ripple
detection (moved-param bounds vs viewport → edge ping event); delete v0
(geometry plus dependent constraints, counts surfaced); undo integration for
all of it; the gesture harness — synthetic event scripts through interact.

Amended during the stage. The hard-pin diagnosis this stage originally
specified — pin the dragged point, solve, read `failed` — cannot work: the
solver reports the set to remove to make the system solvable, and removing
the pin always does that, so the pin is the only thing it ever names. The
pin is the question, not the answer. Replaced by asking which relation, if
suppressed, would let the point reach; `SolveOptions::suppressed` exists for
it and stage 5's conflict walking wants the same primitive.

`--script` playback moved to stage 4. It is a feel tool, not a debugging
tool, and it has nothing to read until the recorder and text format exist —
which this plan already places at the point feel iteration begins in
earnest. Building a player against C++ corpus builders now would be building
it twice.

Amended after the stage 0-4 review (findings 24 and 25). View state is still
the shell's, but how a framing, a pan and a zoom compose into one transform is
render's, beside the fitting it composes over: the arithmetic was living inside
Qt event handlers where no headless test could reach it, which is the same
reason keyboard resolution belongs to the registry. Three behaviours were named
in this scope and not built — pan was accepted and dropped, zoom anchored on the
viewport centre rather than the cursor, and the framing was re-derived from the
document on every sync, so drawing geometry or confirming an offer re-framed the
window mid-gesture. A framing is now fitted once and belongs to the user until
resetView asks for another. The drawn grid also takes its step from the snap
policy rather than a constant of render's own, since a grid is a promise about
where a click lands.

Tests: hit priority table-driven cases; signature correctness incl.
mixed-depth selections; gesture corpus opening set — locality, saturation,
release-commit, no-spring-back, undo-restores-predrag-bytes, delete counts;
ripple event emission on a fixture built to ripple off-screen; drag solve
budget asserted in bench on reference components.

By hand: open demo, drag endpoints, feel the ratio and parallelism hold,
watch resistance light up, undo everything to byte-identical start.

Exit: gesture harness in CI; feel window held; its adjustments frozen as
corpus invariants.

### Stage 4 — drawing and inference

Goal: create geometry with the snap engine proposing constraints, per the
snaps-are-proposed-constraints principle; every gesture gains its numeric
twin.

Scope: the gesture script format, recorder and `--script` playback in the
app, first — everything after it in this stage is a feel window, and a
discovery session that cannot be replayed and watched is a session whose
judgement has to be re-made by hand every time. Held over from stage 3,
where it would have had nothing to read; then tool state machine framework
in interact with live tool-parameter strip data and Esc discipline; line
tool with chained drawing; the snap
engine as constraint-candidate generator over the taxonomy's snap kinds
(endpoint coincidence, on-line, midpoint, horizontal, vertical, parallel,
perpendicular, on-circle; grid as placement-only), producing corrected pose
plus candidate records, with ranking policy and the two-tier commit rule
(coincidence/horizontal/vertical auto-commit; the rest offered); ghost
preview running the same inference that commit runs; post-commit visibility
of inferred constraints with one-action decline, bundled undo; circle and
arc tools — the arc lands as its macro (center-form solver arc plus hidden
construction center point and an on-circle constraint for the through
point; center excluded from default snap by policy); rectangle tool as a
macro emitting constrained primitives (tag and handles deferred to stage 7);
numeric entry sessions — typed digits during a gesture resolve it exactly,
full-precision editing rule, unit parsing through core/units; close-loop
detection emitting the offer event (the action lands in stage 5); registry
core with actions as data (tools, delete, undo registered through it;
projections next stage).

Tests: snap candidate generation table-driven (pointer pose + document →
expected ranked candidates); WYSIWYG property on scripted draws (previewed
set equals committed set); inference precision/recall corpus opening set;
decline and undo-bundling semantics; numeric twin scripts (place, type,
enter → exact slot value; imposition variant creates the dimension);
display-rounding hygiene (formatting never rewrites storage); arc macro
invariants (through-point on circle within tolerance; center follows;
degradation on member deletion); registry conformance sweep v0 (every
action invocable headlessly).

By hand: draw a constrained sketch from nothing; watch ghosts predict
commits; decline an inference; type a length mid-drag; replay a recorded
corpus entry and watch it, which is the check the corpus itself cannot make
— a state can satisfy every asserted invariant and still be visibly wrong,
as the stage 3 branch flip was.

Amended after the stage 0-4 review (findings 18 and 19): the component partition
is rebuilt rather than maintained in place, and undo records carry no seed spans.
Both mechanisms were built, tested and unreachable. Incremental maintenance
returns when a profile asks for it; branch fidelity is already carried by seed
commits being ordinary commands with exact inverses, which is the property the
spans were for.

Amended after the stage 0-4 review (finding 15): the numeric twin here is the
one a creation tool has — place, type, enter — for all four tools including the
arc. Typing during a drag of geometry that already exists is deferred to stage
5, because "the length under adjustment" is ambiguous the moment a vertex
belongs to more than one segment, and the surface that disambiguates it is
stage 5's inline dimension editing. PRINCIPLES still fixes the semantics; this
is an ordering change and nothing else.

Amended after the stage 0-4 review (findings 12 and 20): two format changes
stage 7 and stage 5 need arrived here instead. Horizontal and vertical carry a
nullable reference axis, and tangency carries which end of the arc it holds at.
Both are pure additions that serialize as they always did when unused, and both
were pulled forward for the same reason: the format is versioned but not frozen
until stage 8, and one change to it costs less than two. Stage 7's
rotate-with-retarget and stage 5's imposable catalogue now find the vocabulary
already there rather than amending the format under a feel window.

Amended after the stage 0-4 review: WYSIWYG has a recall half, and the plan only
named the precision one. A relation the commit would drop cannot be ghosted was
built and tested; a relation the commit will declare must be ghosted was not,
and every creation tool's opening click failed it — the click places a point
whose relations wait in pendingSnaps_, so the overlay promised nothing and a
coincidence appeared anyway. The property this stage exits on is both
directions, and the corpus asserts the whole declared set rather than a member
of it, because an assertion that only checks what should be present cannot fail
on what should not.

Exit: a sketch can be authored entirely by hand and gesture corpus covers
drawing; feel window held for snap ranking and auto-commit tiering.

### Stage 5 — action surface and imposition

Goal: the contextually-driven action surface exists as projections of the
registry, the whole catalogue is imposable, and the flagship equivalence —
segments to solid — ships.

Scope: context strip ranked by signature with document-local, deterministic
usage weighting (serialized in an ancillary, droppable document section);
command palette with search; keymap dispatch; applicability predicates
consumed from taxonomy with role-ambiguity prompts (length ratio asks which
way, with preview); imposition actions for the full catalogue with
movement-free capture via core/geom (the near-parallel snap-shut exception
shows its motion); speculative hover preview of any offered constraint
(ghost solve through contexts); conflict downgrade flow (speculative
infeasible → offer reference measurement; conflicting set walkable through
selection); dimension objects — driven/reference toggle, document-anchored
screen-scaled text via the bundled typeface, inline editing as the numeric
twin of dragging; three-strengths presentation for align/distribute/measure;
DOF readout; make-solid — region record over a detected cycle, minimal
even-odd fill on one layer — and heal-and-fill imposing missing
coincidences with epsilon motion shown.

Tests: the applicability conformance sweep at full strength (predicate
agrees with attempted outcome for every kind × generated signature);
movement-free imposition property over random solved documents;
preview-does-not-mutate (document bytes and seeds unchanged after a hover
storm); conflict downgrade path scripted; dimension toggle round-trip;
strip-ranking determinism under fixed usage history; region reference
integrity (vertex drag moves fill with outline — no stale geometry to
test because none exists, asserted by construction via analytic raster
sampling); heal-and-fill scripts (epsilon joints healed, motion ≤ epsilon,
region attached; loop with a crossing rejected with the deferred-case
message per PRINCIPLES).

Amended after the stage 0-4 review. Three things this stage inherits, each
already load-bearing in stage 4:

The conflicting set has to be walked, not rendered. checkCandidate reports what
the solver blamed, and SolveSpace blames the constraint it could not satisfy —
the candidate — while saying nothing about which existing one it disagrees with.
The set is therefore often empty on an inconsistent verdict, and empty means
unattributable rather than unconflicted. Populating it is the suppression walk
stage 3's amendment already introduced SolveOptions::suppressed for; the verdict
alone is what drives the downgrade until then.

The downgrade itself exists and is automatic. A dimension that cannot hold is
already committed as a driven reference measurement rather than as a driving
constraint, checked against a copy of the document with the pending step applied
— stage 4 had no surface to offer the choice on. This stage moves the choice to
the user; the mechanism does not change.

Make-solid refuses cycles shorter than three edges. Two straight segments over
one pair of vertices pass the degree and connectivity tests and walk closed
while enclosing nothing. The bound belongs with boundaries being segments-only
and lifts when arcs become boundary-capable, since two curved edges do enclose a
lens.

Amended during the stage. Seven things the work settled that the plan did not
say, each recorded because the reason is the load-bearing part:

The constraint catalogue's actions are generated, not listed. Twenty-two kinds
at three strengths is sixty-six rows, and a hand-written list of them would be
the sixth projection free to drift that the taxonomy exists to prevent. They are
built from CONSTRAINT_KINDS at first use, which cost one signature change —
`applicable` and `invoke` take the action itself, so one function serves every
generated row — and buys the property the stage was for: a relation added to the
taxonomy reaches the strip, the palette, the keyboard, the script format and the
conformance sweep without a second list being edited. The catalogue is therefore
a runtime table rather than a constexpr one, built in one pass and never touched
again so the string_views it hands out stay valid.

Actions record themselves now, and stage 4's note that nothing records an Action
step is retired. It was true while every action dispatched to a pointer event, a
keystroke or a tool change, each of which records itself — so an action step
re-recorded as the change it caused. Imposition and the fill actions have no such
effect: they are reached by clicking a strip entry, and nothing else about that
click is an input the session sees. Left alone, record → replay → record would
have silently dropped every edit made through the new surface, which is the one
property the script format exists to guarantee.

A mark does not always beat the geometry under it. The hit priority policy puts
adorners above geometry, and read unconditionally that is wrong: a mark sits a
few pixels off the vertex it annotates, well inside that vertex's own hit radius,
so a mark that always won swallowed the press that starts a drag. A stage 3
corpus gesture caught it. Nearer wins — every mark stays reachable without
spending the gesture the tool is mostly used for on the one it occasionally is.

Where a mark sits on screen moved to core, beside GlyphMark. Render draws marks
at their fanned-out positions and hit testing has to pick them there; computing
the fan-out in both places is how a user comes to click a mark and select
nothing. Same rule the pose follows, one layer up: one placement, two readers.

Promoting a reference measurement to driving re-captures its value. A reference
is a live readout of what the geometry is doing, so its slot holds whatever it
last drove at and may be nothing like what it displays. Promotion is imposition,
and imposition moves nothing — carrying the stale slot forward would make a
toggle yank the drawing to a value the user was not looking at. Demotion leaves
the slot alone, or the toggle would be one-way in everything but name.

The typeface is a pinned build input, embedded as bytes rather than resolved at
runtime. DejaVu Sans, from the pinned nixpkgs: freely redistributable, already
in the closure, and a workhorse rather than a style statement, which is what a
dimension label wants to be. Embedded because a font found through fontconfig
makes the sandbox, a raster assertion and a user's machine answer differently,
and a store path baked into the binary would be nix-specific and break the
moment the app is copied. The build refuses to configure without one: an
application whose dimensions are silently blank is worse than one that will not
build.

Carried forward deliberately: a region's fill draws straight edges only. Arcs
are boundary-capable in the taxonomy, and a curved boundary needs the fill
tessellated along the sweep — which lands with the arcs-as-boundaries work the
three-edge minimum is already waiting on, since both are the same question about
what a curved edge encloses. Region styling and the broken-region diagnostic are
stage 6's, as scoped.

By hand: select two segments, see ranked offers, hover to preview, impose;
overdrive a value into conflict and walk the failing set; draw an
almost-closed outline and heal-and-fill it; toggle a dimension between
reference and driving.

Exit: catalogue fully reachable through the surface; flagship demo (draw
outline → solid → drag vertex → fill follows) in the corpus and in
`--script` playback.

### Stage 6 — composition

Goal: the README's layering thesis — occlusion and cut-outs compositional,
never destructive — plus the organizational model.

Scope: layers (order, visibility, lock) and groups (drag-together default);
z-order within layers; alpha-overwrite shapes compositing as region algebra
(punch-through against the accumulated layer, union/intersect composites as
records over live operands); styling properties as slots; lock-equals-pinned
in solves; hidden-still-constrains with the influence indication; region and
tag degradation states rendered (broken-diagnostic rather than dissolution,
one-step restore); bake-at-export stub recorded as the only destructive
path (full export in stage 8).

Amended after the stage 0-4 review (finding 26): groups are not in that
deferral and never were. Membership is organization rather than structure, so a
group that lost one entity still names the others correctly and shrinks rather
than dies — deleting a member is a set-record over the membership today.
Regions and tags are the ones whose contents are load-bearing, and they are what
this stage's degradation states are about.

Amended during the stage 6 build, in the order the decisions were forced.

A lock is a solver group, not a Pin constraint. Locking means "this does not
move", and in a solver world that means its parameters join the fixed set —
GROUP_BASE, the one the workplane sits in, which Slvs_Solve treats as known. A
Pin would be the wrong mechanism twice over: it is a relation the user asked for,
it appears in the failing set, and it can over-constrain, where a lock is
presentation state that must never be able to make a system inconsistent. It
removes unknowns rather than adding equations. Which entities are locked is
derived from the document inside the translation rather than passed in as an
option, because otherwise every caller — the drag path, the diagnose path, each
speculative preview — would have to remember, and forgetting produces geometry
that slides out from under a lock with nothing asserting.

That has a corollary the first corpus run found. A constraint every one of whose
operands is locked has no unknown left to satisfy it, and emitting it anyway
makes the verdict Inconsistent — a lock making a system contradictory, which is
precisely what the paragraph above says it must never do. So a fully frozen
constraint is left out of the system. It is already satisfied, by geometry that
cannot move, so no answer changes. Seeding a locked parameter to the cursor is
the same hazard from the other side: the solver takes a fixed parameter's seed as
its known value, so writing the cursor there would not ask for a move, it would
perform one — the one place a drag target does not go through the solver at all.

Drag-together is a rigid translation outside the solve, not a dragged-set hint. A
group usually joins geometry that is not connected — that is what makes it worth
grouping — so there is no equation relating the members and locality would keep
an unconnected component still. Whole components translate, not the named members
alone: translating one end of a constrained bar would break the bar, and
translating all of it cannot break anything, because every relation in the
catalogue is translation-invariant. The offset is measured from where the grab
started rather than accumulated per frame, so a drag that saturates and comes
back does not leave the carried geometry displaced by the sum of the frames it
spent stuck.

z-order is on regions and nowhere else. Within a layer only fills occlude —
strokes are stroked, and two strokes in a layer do not hide each other — so the
whole of the occlusion order is the layer plus a signed z per region. Putting a z
on every entity would have bought an ordering nobody can see and cost a
restructure of the draw loop into per-entity passes. Raise and lower therefore
act on regions and on layers, and the draw loop is per layer: that layer's fills
composited among themselves, then its strokes, with vertices last over
everything because a handle is an adorner.

Degradation is a shrink, and shrinking made the validator's job smaller rather
than larger. A region that lost a boundary edge keeps the edges it has; a
composite that lost an operand keeps the operands it has; a tag keeps what it
still names. None of them is refused for being thin, because refusing would mean
a deletion could only proceed by taking the higher-order record with it, which is
the silent discard the whole degradation story exists to prevent. Whether a
region is whole enough to draw is therefore not a validation question at all —
it is regionState(), asked in one place, by the renderer and by the diagnostic
readout alike.

That forced a real fix rather than a new feature. deletionStep grew a
whole-selection overload, because the per-entity cascades a multi-selection used
to be stitched together from each computed their own shrink of the same region,
each dropping a different edge, and the deduplication in front of them kept
whichever came first. The step then failed on the removals the surviving shrink
had not accounted for and rolled back, so deleting two edges of a filled loop
silently did nothing. Same shape as the group-shrink note in stage 1: a shrink
has to be computed over the whole doomed set or it is computed wrong.

Walking a region's boundary moved to core, and became topological on the way.
Render was matching endpoints by coordinate, which is what it had; the
degradation query needs the same answer and must not be a second implementation
of it. Coincidence is what decides that two ends are one joint — corners are
separate points joined by a relation, so identity matching would call every
rectangle broken, and coordinate matching calls an unsolved document broken and
flickers. boundaryRing asks the document and takes no pose.

Booleans are records over live operands, and a region is named by selecting what
bounds it. No third selection list: a fill has no handle of its own, which is the
same reason it has no geometry of its own, so an outline counts as selected when
every edge it names is and a composite counts when every operand does. An operand
belongs to at most one composite, or it would draw twice and lifting it back out
would have two answers. Punch-through draws inside a saved layer, so an alpha
overwrite carves what its layer accumulated rather than cutting to the canvas —
cutting through would make a hole's effect depend on what happened to be
underneath it, which is the destructive reading of a boolean rather than the
compositional one.

Styling values are slots, and entities gained a style reference so strokes are
covered by that. Opacity joined stroke width as a slot; the colours stay packed
RGBA because they are not quantities arithmetic applies to. The payoff is the one
the slot thread always promised: a named document parameter drives every fill's
transparency at once, and scrubbing it is an ordinary value edit rather than a
styling system of its own.

The bake is a value-producing projection in core, and the polygon boolean is not
in it. Baking never mutates a document and there is no in-document bake; what it
returns is rings tagged with the operation that joins them, plus counts of what
was lost. Resolving a union or an intersect into a single outline needs a path
library, which belongs with the exporter in stage 8 — core has no business
growing one, and a half-resolved boolean baked now would be the lossy converter
this project refuses to build.

Actions record under the registry's name for them, never a name of their own.
Layer visibility is two actions and was briefly recorded as one with a flag; the
step named something the registry does not have, so replay dropped it, the script
still parsed, and the edit was gone. Record → replay → record held on both sides
because both sides had lost it. The corpus gesture now checks that every step it
recorded names a registered action.

Tests: analytic raster sampling for fills, punches, and composite stacks
(inside/outside points, transparency where punched, layer-order
permutations); lock-as-pin (locked params bit-unchanged under connected
drags, solver treats as fixed); hidden-influence event emitted when an
invisible operand moves a visible result; degradation and restore scripts
(delete an edge of a filled loop → diagnostic state → undo restores bytes);
cross-layer constraints solve identically to same-layer (partition is
layer-blind).

Carried forward deliberately. A region's fill still draws straight edges only,
for the reason stage 5 recorded and this stage did not change: a curved boundary
needs the fill tessellated along the sweep, which lands with arcs-as-boundaries
alongside the three-edge minimum. Tag degradation is modelled, queryable and
tested, but nothing renders a broken tag, because nothing creates a tag — the
rectangle tool's tag is stage 7's, and it is the only producer there will be, so
the affordance and its degraded state land together rather than one of them
landing against a record no gesture can make.

By hand: build a cut-out that stays constrained to what it cuts; drag the
plate, hole follows; hide the driver layer and watch the influence
indicator; lock a layer and feel geometry pin.

Exit: composition model exercised by corpus; no destructive boolean exists
anywhere in the document model.

Amended after the stage 5–6 review, docs/REVIEW.md, whose correctness findings
are all closed. Four of them changed what this stage says it built.

The bake returns a tree, not a flat list of tagged rings. One group per
composite naming the group it is an operand of, because Intersect(A, Union(C,D))
is not A∩C∩D and a flat list said it was. Rings arrive in operand order, which
is the only thing marking a subtract's minuend, and curves arrive tessellated at
a fixed angular step — circles and arcs were stroked on every screen frame and
absent from the export with nothing counting them, which broke the loss report's
own contract. Resolving the tree is still the exporter's, in stage 8.

Which region a subtract subtracts from is decided by occlusion order rather than
by creation order, and region.subtract takes an argument for the other reading.
The plan had booleans as records over live operands and said nothing about
operand order, which left it to whichever region was made first — a role
ambiguity of exactly the kind PRINCIPLES sends to the surface, on a kind where
the wrong reading is far more visible than length-ratio's. The default now
matches what the user is looking at; a surface that asks is still to build.

A fill takes the layer of the outline that defines it. Stage 5 deferred the
choice here and this stage landed layers without coming back, so every fill went
to the lowest-ID layer record and could be split from its own geometry by hiding
a layer.

Relations delete through the same selection machinery geometry does, and a
constraint removal shrinks the tags built on it exactly as an entity removal
shrinks the regions bounded by it. That is the model half of stage 7's "tag
dissolution when defining constraints break", landed early because finding 3 —
a walked conflict set that could not be deleted — needed the same cascade.

The movement-free property now runs over random solved documents, as this plan
asked for it and as the fixtures could not. It found two measurements that were
not the solver's — point-line distance captured unsigned where the solver reads
it signed, and an angle's supplementary form captured holding the default form's
value — both of which moved geometry on the path whose whole promise is that it
does not. The rule finding 7 established generalises: a geometric measurement is
the solver's measurement or it is a bug waiting for a configuration, and the way
to know is to ask a random document rather than a chosen one.

Stage 5's surfaces caught up with what stage 5 said they were. The hover preview
ghosts the geometry rather than only reporting a verdict, which is what PLANS
scoped and PRINCIPLES calls the thing that makes the catalogue learnable by
looking; the downgrade is an offer on the strip rather than a computed boolean
nothing read; marks follow their operands out of sight when a layer is hidden;
and "is this region selected" has one answer, in core, where render and the
region actions both ask it.

The review's model holes are closed with it. Layer and style removal refuse
while anything names them and have deletion steps that empty them first, which
they lacked entirely; the state they used to leave was a document that
serialized and would not load. And the taxonomy learned that role ambiguity has
two shapes, not one. Order sensitivity — does swapping these two change what it
says — is the right question for length-ratio and the wrong one for equal-angle,
where swapping never changes anything and the pairing is what nobody asked
about. `operandGroupSize` says the slots group; the surface enumerates the
groupings and asks. Stage 7's compound relations are the next kinds likely to
need it.

### Stage 7 — structure operations

Goal: the operations that treat constrained structure as structure —
transform, copy, compound — land with the semantics PRINCIPLES fixed for
them.

Scope: rotate and uniform scale of selections (exact isometry rewrite of
seeds, then re-solve); the axis-constraint question surfaced once with
preview (retarget horizontal/vertical to a cluster frame — construction
geometry — or keep document axes; the nullable reference axis this needs is
already in the format, per the stage 4 amendment, so the work here is the
surface and the flow rather than a format change under a feel window);
scale-the-values versus let-them-resist for absolute dimensions (slot
rewrite by factor); non-uniform scale refused
in-model, available only at export-bake; copy/paste/duplicate with
internal-constraint closure, fresh IDs, boundary-constraint drops indicated;
duplicate-with-offset as the seed of patterns; compound relations —
distribute, mirror — as macro expansions with tags; the rectangle tool
upgraded with its tag, handles, and width/height panel; tag dissolution
when defining constraints break, leaving all primitives and remaining
constraints untouched.

Tests: isometry property (rotating a rigidly constrained cluster leaves
internal residuals at zero before re-solve; re-solve is identity within
tolerance); retarget flow scripts (both answers produce their documented
outcomes; the kept-axes path yields the expected resistance); ratio
constraints invariant under uniform scale while distances rescale only via
scale-the-values (slot values multiplied, re-solve identity); copy
isomorphism property (kind-preserving bijection, disjoint IDs, boundary
drop count reported); compound expansion equals hand-built primitive set
byte-for-byte modulo IDs; tag dissolution leaves a byte-stable remainder;
rectangle handles drive the underlying slots and dissolve honestly.

By hand: rotate a rectangle and choose each axis answer; scale a dimensioned
drawing both ways; duplicate a constrained sub-assembly and verify it holds
together; break a rectangle into ordinary geometry by deleting one
perpendicularity.

Exit: transform and copy semantics in corpus; compound tags dissolving
gracefully under scripted abuse.

Landed. The transforms are exact seed rewrites in core/transform, so the
isometry property is checkable before anything re-solves rather than being a
statement about Newton's tolerance. Both questions ride as arguments on their
actions and therefore round-trip through scripts: an answer that failed to
record would replay the other one, and the script would still be byte-identical
to itself while describing a different drawing. Copy is core/copy, one bijection
serving duplicate and mirror alike. Compounds are core/compound and emit nothing
the catalogue did not already have. The rectangle's tag, handles and size panel
are core/tags, asked in one place by render, hit testing and the panel.

A high-effort adversarial review over the finished diff found ten defects, all
fixed and all now carrying a regression test: the retarget frame was free and
added four degrees of freedom (it is pinned); the transform centre came from the
pose while the rewrite is in seeds; a negative scale factor was unguarded;
distribute ordered its points by ID rather than along the run; edgeDimension
matched point identity where a corner is two coincident points; the handle drag
suppressed dimensions on corner membership rather than on the handle being
offered, never wrote back on the numeric path, and flattened expression slots;
the panel committed a driving dimension unchecked; and a broken tag was carried
through a copy. The pattern worth carrying into stage 8 is that eight of the ten
were on the two new surfaces where a mechanism was inferred from geometry rather
than from what the user could see — the handle and the frame.

Arcs-as-boundaries landed after the stage, out of sequence, because live testing
found the symptom: an outline with an arc for one corner never offered make-solid.
The cause was the segments-only guard stage 5 put in findBoundaryCycle, whose
stated condition — "arcs join them once their macro lands" — had been met since
stage 4. Lifting it alone would not have been safe. Three walks encoded "the two
ends of a boundary edge" independently, and two of them read points[0] and
points[1], which is an arc's centre and its start; the guard had been hiding
that rather than deferring it. So the fix is one shared `boundaryEnds` in
core/records.h, one shared `enclosesArea` replacing the three-edge constant, a
boundary ring that carries the traversal direction rather than bare corners, and
fill tessellation along the sweep in both render and the bake. Circles came with
it: a closed curve bounds alone, takes no part in the joint walk, and is answered
from the seed — which is also what decides, unambiguously, which of two closed
things a click meant. The crossing diagnostic followed, since it was the blind spot the change opened:
it tested segment pairs only, so an arc crossing a segment fell back to the
silence the diagnostic exists to break. It now covers every pairing of straight
and curved, with tangent, concentric and nested cases reporting nothing for the
reason parallel segments do. Heal-and-fill's own three-edge minimum went through
`enclosesArea` at the same time — an arc and a chord a hair apart are two edges
that will enclose once the gap is shut, and a hard three would have refused to
offer the heal that make-solid then accepts.

Two more surfaces curves now reach were closed at the same time, both found by
auditing every reader that had assumed an edge is a straight segment. The
broken-region diagnostic stroked through Pose::segment alone, so a degraded
curved boundary drew nothing — a circle-bounded fill evaporated with no
diagnostic, the exact silence that state exists to prevent, arriving through the
code written to prevent it. And fitView bounded from defining points only, so a
document that is one filled circle framed from its centre; a circle alone is a
complete drawing now that it bounds a region. Both are drawing-side and neither
touches the model.

The last one is the numeric twin for grabbing an arc endpoint, which the drag
dimension collector had never offered because it walked segments only. It was
harmless while an arc endpoint was rarely a drag target and became a real gap
once curves bound regions and the endpoint is a corner a user drags in anger.
Grabbing it now offers radius and chord, Tabbed exactly as a shared vertex's two
lengths are — so every gesture has its numeric twin again, the arc included from
both sides.

The remaining deferral is unchanged and is the honest one: filling an area
enclosed by edges that cross still needs explicit intersection points. The
diagnostic now names that case wherever it occurs rather than only among
segments.

Two things settled differently from the sketch above. Non-uniform scale is a
permanently dimmed palette row rather than an applicable action that refuses,
because applicable-and-refusing would break the property that an applicable
action runs — the one that makes the registry trustworthy. And "handles drive
the underlying slots" turned out to need suppression on the drag rather than a
second mechanism: the handle suppresses the dimensions it is about, tracks
freely, and commits their values from where the corner landed, which is the same
value edit the panel performs from the other direction.

### Stage 8 — durability and scale

Goal: the document format freezes, over-budget solving degrades gracefully,
and the tool speaks to the outside world.

Scope: format freeze v1 — written spec in docs/, migration policy (shims
only after freeze; corpus regeneration before), fuzzed round-trips,
forward-compat corpus with unknown records from a synthetic future version;
async solve path — scheduler, worker pool, snapshot-in/generation-out over
lock-free rings, stale-result discard, no partial blends, synchronous path
unchanged and preferred under budget (async threshold is a policy); bench
suite as a CI-adjacent gate with ratio-vs-baseline comparisons; SVG export
as bake (solved geometry, regions to paths, alpha overwrite to masks;
structural XML assertions with numeric tolerance); SVG import as trace
(subset; geometry arrives unconstrained; inference-on-import explicitly
deferred).

Tests: fuzz round-trip (random documents through serialize/load cycles,
byte-fixed-point after first cycle); forward-compat (future-versioned files
shed nothing through open/save); async determinism (injected-delay solver
hook — results identical to synchronous within tolerance, stale generations
provably dropped, UI-state fixture never observes a partial solution);
budget gates on reference scenes; export assertions incl. mask semantics
for punched regions; import produces expected unconstrained record sets.

By hand: save/load heavily edited documents across the stage boundary; drag
inside an artificially slowed component and confirm the last coherent state
holds with no blend; open an export in an external viewer.

Exit: format spec published in docs/; async path exercised by corpus;
export/import round-trip demos recorded.

Landed. The four pillars are in and green through `nix flake check`: the format
froze with a written spec, the async path degrades over-budget solving off the UI
thread, the bench gained a deterministic gate, and SVG export and import speak to
the outside world.

The format froze at version 0 rather than bumping a number. The freeze is a
policy and a spec, not a version change: "v1" is the product version, and the
on-disk format has been version 0 since stage 1 and stays there. docs/FORMAT.md
is the spec, field for field against persist.cpp, with the migration policy at
its end — additive change keeps the version and rides the unknown-record
mechanism, a breaking change bumps the version and ships a read shim, and a
greater version than the reader's is refused whole. Forward compatibility is
therefore exactly what PRINCIPLES named it, unknown record kinds surviving a
round-trip, and tests/corpus/future.paro is a version-0 document carrying record
kinds no build here defines, which the corpus opens and re-saves shedding
nothing. The fuzz round-trip reaches a byte fixed point after one cycle over the
whole vocabulary, and the rich fixture the freeze suite calls its contract was
broadened to exercise every branch it had been skipping — both composite
operators, all three tag kinds, a non-empty usage section. A seed list longer
than the kind owns is now refused like an over-long point list, closing the one
load-path asymmetry a review found.

The async path is snapshot-in, generation-out over lock-free SPSC rings, in
solve/spsc.h and solve/scheduler. A submit hands a worker an immutable document
snapshot and a component context; a result carries the whole solved context and
its generation; the UI drains, keeps the freshest per component, and never sees a
partial solution because a context is filled entirely before it is pushed. The
synchronous path is preferred under budget and is byte-identical when async is
off — the scheduler is opt-in and null by default — which is what keeps the
gesture corpus deterministic. The determinism, stale-drop and no-partial-blend
properties are exercised both inline and on real worker threads through the
injected-delay hook the plan named.

One deviation was forced and is load-bearing. The vendored solver's C wrapper
keeps a file-global scratch system (`static System SYS`, `static ParamSet
dragged`), so two Slvs_Solve calls in flight at once corrupt each other —
concurrent solving is not possible without patching the pinned submodule, which
is out of this stage's scope. So a solver-serialization gate guards the solver
call: a mutex taken only while a scheduler with real workers is alive, so the
synchronous-only build never touches it and pays nothing. This is the one lock
that crosses threads in our code, and it is a substrate constraint rather than a
document race — it protects the third-party solver's global scratch, not any
state the no-locks rule is about, and it is held only for the solve. The
consequence, stated plainly: async buys the UI-never-blocks and last-coherent-pose
properties, not a parallel-solve speedup, because the solver still runs one at a
time. When the vendored solver is made thread-safe (its globals thread-local, the
same shape platformbase already gives its mimalloc arena), the gate lifts and the
pool parallelises with no change above the seam.

Staleness is caught twice. Per-component generations order results within a
component, and a per-refresh document epoch — bumped every edit — tags each
submission so a solve of a document the user has since edited is dropped rather
than applied over the newer one, which the per-component key alone cannot catch
because an edit can move an entity to a different component. And a failed or
non-converged async solve is not applied at all: the component holds its last
coherent pose exactly as the synchronous path holds its committed seeds, while
its over-constraint still reaches the readout. That last point was a review
finding — the first cut applied every async result unconditionally, which would
have walked geometry through its own constraints, the one thing the sync path
forbids.

The bench stayed a solve bench and gained a deterministic half. The existing
ratio-vs-baseline timing comparison is the CI-adjacent gate, run on demand
because a wall-clock gate in the sandbox is a flake generator, which was already
the decided design. Beside it now is an exact arena-bytes gate: the translation
scratch's high-water mark is a function of the document, not the clock, so a
change there is a real regression in how much a solve allocates rather than a
timing wobble — the half that is safe to automate. Frame-time benchmarking on
rendered reference scenes is deferred, because it needs Skia in the bench and the
solver is the variable term the budget actually turns on.

SVG export is the bake carried the last mile, in core/svg. The region algebra
that has no SVG equivalent is expressed structurally rather than resolved by a
polygon-boolean library core has no business growing: a fill is a path, a subtract
and an alpha-overwrite are masks, an intersect is a nested clip, a union is
painting the operands. The masks are op-respecting — a subtract flips its
subtrahends to the opposite polarity, so a nested or punched composite carves the
region it actually is rather than the union of its rings, which two review
findings caught the first cut getting wrong. Structural expression is exact when
operands nest, the ordinary hole-in-a-plate case, and can only approximate a
pathological non-nesting arrangement or an intersect inside a mask; those are
counted in the file's own loss-report comment rather than shipped as a wrong shape
with no word said. A punch carves what its layer accumulated below it in z-order
and never a stroke, which is the incremental-masking fix a review forced. Import
is a trace and deliberately less: a straight-line-and-circle subset to
unconstrained geometry, inference-on-import deferred, everything outside the
subset skipped whole and counted. Both directions share the document-up /
SVG-down y convention, so an export re-imported lands where it started. The app
grew `--export` and `--import`, headless projections that run and exit before any
window, and tests/corpus/import.svg plus the round-trip demo are the recorded
artefacts the exit asked for.

Three high-effort adversarial reviews over the finished diff shaped the final
state, and the pattern from stage 7 held: the defects clustered on the new
surfaces where behaviour was inferred rather than shown — the async result
application and the SVG mask compositor. Every finding worth acting on is fixed
and carries a regression test. What is deferred is deferred honestly and named
here: true parallel solving behind a thread-safe solver, frame-time benchmarking,
and the deep-nested-composite and external-path-notation corners of the SVG
converter, each counted or documented rather than left silent.

### Track R — renderer (parallel, entry after stage 6)

The QQuickPaintedItem CPU path is a known shortcut. This track swaps it
behind the paint-document-plus-overlay interface without touching anything
above render.

Scope: spike first — candidates are Skia Ganesh sharing Qt's RHI device
(Vulkan or GL, depending on what the pinned Skia build exposes — to verify
in-spike), versus staying CPU with damage rectangles and texture upload.
Decision recorded in docs/, then implementation behind the unchanged
interface, `QQuickRhiItem` on the shell side.

Tests: pixel-parity harness between CPU and GPU paths (analytic samples
with tolerance, run over the raster corpus); frame budget on reference
scenes at high DPI; fallback path (software) stays green in CI, which
cannot assume a GPU.

Exit: GPU path default where available, CPU fallback intact, parity harness
in the corpus.

## Sequencing rationale

The order cashes the load-bearing interactions from PRINCIPLES at the
earliest moment each becomes testable, and no later:

| Thread | First cashed | Guarded by |
|---|---|---|
| taxonomy spine | stage 1 tables | conformance sweeps (2, 4, 5) |
| seeds | stage 1 journal shape; stage 2 store | branch fixture, undo byte-identity, scrub sweep |
| slots | stage 1 | numeric twin scripts (4), scale-the-values (7) |
| snapshots/contexts | stage 2 value types | preview-does-not-mutate (5), async determinism (8) |
| topology | stage 1 graph | marked-vs-rebuild property, regions (5, 6) |
| registry | stage 4 core, 5 projections | headless-invocable sweep, applicability conformance |
| no-silent-changes | stage 3 ripple, 4 inference visibility, 5 motion display, 6 influence | corpus scripts per surface |

Orderings that must not be traded away: persist and undo exist before the
solver (stage 1) so model semantics are pinned independently; the semantics
suite exists before any interaction (stage 2) so feel work never debugs
math; the gesture harness exists before drawing tools (stage 3) so every
tool lands with its invariants; the registry exists before the second UI
surface (4 before 5) so surfaces are born as projections; seeds ride undo
from the first journal entry, because retrofitting branch fidelity into an
existing undo stream is the expensive path.

## Risks and de-risking

| Risk | Probe | Contingency |
|---|---|---|
| translation overhead dominates warm solves | stage 2 bench isolates translate vs solve | persistent translation caches keyed by component version, behind the solve seam |
| saturation attribution unclear or noisy | settled in stage 3: the hard pin only ever names itself, so attribution is a leave-one-out counterfactual on absolute travel | degrade to highlighting the constraint chain of the dragged component |
| inference feels naggy or rigidifies documents | tests/gestures/inference.cpp, exact declared sets either side of each tolerance edge; decline counts in dev builds | tier thresholds are policies; worst case, parallel/perpendicular drop to offer-only |
| Skia GPU sharing with Qt RHI fails on pinned builds | track R spike before commitment | CPU raster + damage rects + upload; interface already isolates the choice |
| taxonomy tables ossify or leak abstraction | three-consumers rule before any generalization | tables are plain data; projections are functions; no codegen until proven need |
| SolveSpace redundancy quirks (`REDUNDANT_OKAY`) | stage 2 redundancy variants per catalogue entry | creation-time speculative check owns the policy; solver status never surfaces raw |
| format churn during feel iteration | versioned from stage 1; freeze deferred to 8 | pre-freeze breaking changes regenerate corpus; no migration shims until freeze |
| arc macro center semantics confuse selection | stage 4 design note + policy-demoted snap | center visibility becomes a role-based policy toggle |

## Deferred hooks

Parked per PRINCIPLES, each with the seam it will hang from when its time
comes: animation (slots grow track kinds; contexts already evaluate
per-sample), beziers (cubic entity plus existing tangency constraints; a new
taxonomy row, not an architecture event), public scripting (the registry
goes public; actions are already data), collaboration (deterministic
serialization and persistent IDs are the prerequisites and both freeze at
stage 8), solver replacement (everything behind solve/; the semantics suite
is the acceptance test a replacement must pass), Windows (per README, a
packaging effort behind `pkgsCross`, not a code event).
