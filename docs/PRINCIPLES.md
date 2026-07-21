# Principles

This document fixes the decisions that are expensive to change: what the
document is, which equivalences the tool honors, how interaction behaves under
a solver, and where the scope fences sit. The implementation plan is built on
top of it in a second pass. Feel — hover priorities, inference thresholds,
glyph density — is explicitly left to iterative discovery; what is recorded
here is the machinery that makes that iteration cheap and safe.

The reference points are README.md's thesis (proportion, spacing, alignment
and parallelism as primitives; geometry as their consequence) and the wired
toolchain: SolveSpace's solver behind `slvs.h`, Skia raster, Qt Quick shell,
Eigen transforms, mimalloc heaps.

## Vocabulary

Terms used throughout, fixed here so the prose can stay terse.

- **declaration** — the authored state: entities, constraints, values, roles,
  tags. What the user meant.
- **solved state** — parameter values the solver produced from a declaration.
- **seed** — prior solved values fed into the next solve. Selects among
  multiple mathematically valid solutions.
- **component** — a connected subgraph of the constraint graph, solvable
  independently of every other component.
- **region** — a fill record that references a closed cycle of edges. Not a
  copy of them.
- **role** — a presentation flag on ordinary geometry (construction, guide).
  Never a distinct type.
- **tag** — a revocable macro identity over primitives (rectangle,
  distribution). Convenience, never load-bearing.
- **slot** — the value cell of a constraint or style property. Holds a
  constant today; an expression or keyframe track later.
- **signature** — the typed multiset of the current selection, e.g.
  {segment, segment} or {point, arc}. The key for contextual UI.
- **adorner** — screen-space overlay: handles, constraint glyphs, dimension
  text, snap marks.

## The document is a program

The document is the declaration layer. Geometry on screen is derived output —
a cache of the last solve — and every editing operation writes declarations,
never solved coordinates directly. A drag does not set a point to (x, y); it
asks the solver for the nearest legal state in which that point sits at
(x, y). This is the single deepest commitment: rendering, hit-testing and
export read the cache; editing writes the program.

One consequence is non-obvious and load-bearing: the solved state is itself
semantic, not merely cache. Constraint systems generically admit multiple
solutions — reflections, elbow-up versus elbow-down, crossed versus uncrossed
quadrilaterals — and Newton iteration converges to the solution nearest its
seed. Which branch the user is looking at is therefore part of the document.
The persistent model is declarations plus seeds. A file reopened, an undo
applied, or a timeline scrubbed backwards must re-solve from recorded seeds,
or the geometry can silently flip to a mirror solution that satisfies every
constraint and still betrays the user. Seeds are serialized, seeds ride in
undo records, and "re-solve from scratch" is never a correctness-neutral
operation.

The solver is a stateless function, matching the shape of `slvs.h`: build a
system, call `Slvs_Solve`, read parameters back. Solver handles are transient
per invocation. Document identity is separate and permanent: every entity,
constraint, region, and tag carries a persistent ID, never reused, never
positional, because constraints, regions, slots and undo records all refer
across time. The translation from document IDs to solver handles is
regenerated per solve and owned by the solve layer.

Determinism is a document property. The same declaration and seeds must solve
to the same geometry on every machine and every run: stable iteration orders,
no behavior keyed on pointer values or hash-map order, stable serialization
order. This is what makes the selftest philosophy (assert geometry against
declared constraints, not solver status codes) extensible to the whole
product, and it is a precondition for scrubbing, undo fidelity, and any
future collaboration.

## Under-constraint is the normal state

CAD culture treats a fully constrained sketch as the goal and remaining
degrees of freedom as a to-do list. Here the polarity inverts: everything a
user draws is born fully free, constraints accrete opportunistically, and
most documents will live and ship under-constrained. Degrees of freedom are
not debt; they are the direct-manipulation surface. A free DOF is exactly a
thing the user can still push by hand.

This has consequences the CAD lineage does not prepare for:

- The DOF count is a property to display calmly, never a progress bar or a
  warning. `Slvs_System.dof` provides it per solve; per-selection freedom is
  worth surfacing later, but the primary probe for "what can still move" is
  dragging itself, which must therefore be safe, cheap and continuous.
- Over-constraint is handled at two moments. At creation: an action that
  would make the system inconsistent or redundant is caught before commit —
  the candidate constraint is solved speculatively, and on failure the action
  surface offers the downgrade (add as a driven reference measurement instead
  of a driving constraint) with the conflicting constraints highlighted. At
  edit (a value change makes an existing system infeasible): the document
  stays editable, geometry holds the last feasible solution, and the failing
  set — which the solver reports per constraint via `failed` — becomes a
  selection-like highlight the user can walk.
- Diagnostics use the ordinary channels. A conflict is presented as a
  selectable set of constraints plus tinted geometry, not a modal dialog.
  There is no error state that suspends editing; there are only states with
  more or less diagnostic adornment.
- Redundant-but-consistent (`SLVS_RESULT_REDUNDANT_OKAY`) is tolerated at
  solve time but flagged at creation time, because redundancy is where later
  edits go to die: two constraints that agree today will disagree after the
  next value edit, and the user who added the second one was told nothing.

## One representation, many presentations

The user-facing complaint this project exists to fix: tools impose separate
workflows for things that are mathematically the same object. Inkscape's
rectangle-versus-path split, sketch-versus-shape mode walls, destructive
boolean operations — each is a second representation of something that
already existed, connected by a lossy converter. The converter is where
constraints die and intuition breaks.

The principle: where two user-facing notions are the same mathematics with
different bookkeeping — a **representation identity** — they must be one
object in the model, presented differently. The corollary is the maintenance
guard: equivalences are honored by never creating the second representation,
not by writing translators between representations. If a feature seems to
need a converter, the design question is why the two things are not already
the same object. A translation layer between live representations is the
smell that announces future hell.

The test for what gets pre-emptive support: representation identities are in
(closed outline ↔ region boundary; shape-tool output ↔ constrained
primitives; guide ↔ construction-role line; alignment command ↔ alignment
constraint; boolean cut ↔ compositional region algebra). Semantic analogies —
"X is conceptually like Y" without shared math — are out; those are new
features and must argue for themselves. Cases discovered in real usage get
the same test applied then, per the project's stated tolerance for
equivalences that fall through initially.

### Worked example: segments to solid

The flagship case. A user draws four segments; endpoint snapping has made the
joints coincident as they drew; the loop closes. Making this a solid is not a
conversion:

- A region record is created referencing the cycle of edge IDs. No geometry
  is copied, no path object is synthesized, no constraint is touched. The
  outline's constraints keep operating; drag a vertex and the fill follows,
  because the fill has no geometry of its own to go stale.
- The inverse is deleting the region record. The outline and every constraint
  survive untouched. Round-tripping is exact because nothing was translated
  in either direction.
- Closure is topological, not visual: a cycle in the coincidence graph. If
  the user asks to fill an area that is visually closed but topologically
  open — endpoints near but not coincident — the offer is heal-and-fill:
  impose the missing coincidence constraints (moving geometry by the epsilon
  the user already couldn't see), then attach the region. The gap between
  looks-closed and is-closed is bridged by explicit constraint imposition,
  never by a parallel pixel-flood representation of fill.
- Deleting an edge of a filled loop degrades the region: it renders in a
  broken-diagnostic state (or dissolves with a one-step undo), it does not
  block the deletion and it is not silently discarded. Enclosed areas formed
  by crossing segments rather than shared endpoints are a known later case of
  the same rule — instantiate explicit intersection points (a construction
  point carrying two on-line constraints) and build the cycle through them —
  deferred, not forgotten.

### Shape tools are macros

A rectangle tool does not create a rectangle type. It emits four segments,
four coincidences, two horizontal/vertical (or parallel/perpendicular)
constraints, a region, and a tag. The tag supplies rectangle-specific
affordances — corner handles, a width/height panel — for as long as the
defining constraints hold, and dissolves gracefully the moment an edit breaks
them, leaving perfectly ordinary constrained geometry. Nothing is lost at
dissolution because the tag never owned anything. There is no convert-to-path
cliff anywhere in the tool: the cliff is what the macro-plus-tag design
deletes. Compound relations follow the same pattern: distribute-evenly or
mirror emit primitive constraints plus a tag that lets the set be edited as
one thing while it lasts. The solver only ever sees primitives.

Tags are recorded at creation, not recognized after the fact. Recognition
(user hand-builds four constrained segments, tool offers rectangle handles)
is deliberately deferred: recording is cheap and honest, recognition is a
heuristic feature that can come later without model change.

### Booleans are composition

Per the README's layering goal, occlusion and cut-outs are compositional:
a shape can carve visibility from what is below it (alpha overwrite) while
remaining a live, constrained, editable object. Union, subtract and intersect
are display-tree algebra over live regions — the operands persist, can be
constrained to each other (the hole stays concentric with the plate), and can
be lifted back out. Destructive path booleans, where operands are consumed to
mint a new path, exist only as an explicit bake at export. Baking is honest
about being lossy and is never the in-document representation.

Layers and groups are organization, not semantics: render order, visibility,
lock state, drag-together defaults. Constraints cross layers and groups
freely. Two interactions need fixing now: a locked layer's geometry enters
solves as pinned (locking means "this does not move", which in a solver world
means its parameters join the locked set); a hidden layer's geometry still
constrains — with an indication whenever an invisible thing influenced a
visible result, per the no-silent-changes policy below.

### Construction is a role

Guides, construction lines, reference circles: ordinary geometry with a
render role. They participate in constraints identically, are excluded from
regions and export, and sit at reduced hit priority. Because a guide is just
a construction-role line, every guide capability (angled guides, guide
circles, constrained guides, guides at a ratio of the page) is free — the
features fall out of not having a guide type. An axis or frame is likewise
construction geometry, which section Transforms builds on.

### Relations have three strengths

Any relation the tool can compute exists at three strengths: measured once
(align these now, remember nothing), imposed (create the constraint),
displayed (live readout that never drives). Align-left, distribute, measure
distance, dimension — all are one semantic at different strengths, and the
action surface presents them as one action with a strength choice, not three
scattered features. A reference dimension and a driving dimension are the
same object with a toggle; flipping the toggle is how measurement is promoted
to intent (and demoted at over-constraint downgrades). This collapses three
menus of legacy UI into one grammar and is the practical face of
measure-impose duality.

## Interaction grammar

### Drag is a solve

Interactive drag maps to the solver's native mechanism: the dragged point's
parameters go into the `dragged` set (`Slvs_System.dragged`), the solver
favors solutions keeping them near the cursor, and everything else moves as
little as the constraints allow. Multi-selection drags put all selected
parameters in the set. The user-facing pin ("this stays put") is the hard
form, `SLVS_C_WHERE_DRAGGED`, one keystroke, visible like any constraint.

Feel rules that are commitments, not tuning:

- Release commits what is on screen. The solved state at mouse-up becomes the
  new seeds. Nothing springs back.
- An unreachable drag target saturates instead of failing: geometry rides the
  feasible boundary while the cursor overshoots, and the constraints doing
  the resisting light up while saturated. This is how users discover
  constraints by feel — resistance with attribution, not refusal.
- Locality is preferred: seeds plus component partition mean a drag re-solves
  only its component, and distant geometry cannot move at all unless
  connected. Within a component, favoring minimal displacement is a solver
  seeding policy, to be tuned but never abandoned.
- If a solve moves anything outside the viewport, the edge of the screen says
  so (a directional ping). Off-screen consequence without indication is how
  parametric tools lose trust.

### Imposition is movement-free

Adding a constraint to existing geometry captures the current measured value
and moves nothing: impose a distance, it becomes the current distance; impose
parallelism on two nearly parallel lines — the exception that proves the rule
— snaps the small angle shut, and that residual motion is shown. Geometry
moves when values are edited or things are dragged, not when intent is
declared. The trust rule generalizing this: declaring intent about the
present never rewrites the present. (Creation-time inference is exempt
because the geometry was placed at the snapped pose to begin with;
heal-and-fill is exempt because its epsilon motion is the explicit, offered
point of the action.)

### Every gesture has a numeric twin

Any drag can be finished with digits: start dragging a vertex, type 45,
Enter — the length under adjustment becomes exactly 45, with the option (one
more key) of imposing it as a driving dimension. Any numeric field is
scrubbable, which is a drag whose solve loop is the same one the canvas uses.
Approximate gesture and exact entry are two entrances to the same edit, never
two tools. The demo slider driving len(A)/len(B) already embodies the
pattern: a continuous control scrubbing a constraint value through re-solves.

Numeric input is expression-shaped from day one: every constraint and style
value is a slot, and a constant is the trivial expression. The v1 language is
deliberately minimal — numbers, units, arithmetic, references to named
document parameters — but the slot indirection is not deferrable, because
dimensions, scrub, unit display, later expressions and later keyframes all
hang off it, and retrofitting slots under thousands of raw doubles is the
expensive path. Two hygiene rules: display rounding never round-trips into
stored values (edit sessions open on the full-precision value, not the
rendered text), and units convert at exactly one boundary, presentation.

Measurements never drive slots. A slot that needs to depend on geometry is
not an expression reading a measurement — it is a constraint, where the
feedback loop belongs to the solver. This keeps the value-dependency graph
acyclic by construction and closes off the measure-drive-measure oscillation
that plagues spreadsheet-coupled CAD.

### Inference: snaps are proposed constraints

The bridge between freehand drawing and the parametric layer is that a snap
is not a coordinate correction; it is a constraint candidate that placement
commits. Snap-to-endpoint commits coincidence; snap-to-horizontal commits
horizontal; snap-parallel commits parallel. The snap engine is therefore a
constraint-candidate generator sharing the constraint taxonomy — designing it
as a geometric corrector and bolting inference on later would build the
converter this project refuses to build.

Inference discipline, because helpful rigidity is its own failure mode:

- Only the strongest candidates auto-commit (coincidence, horizontal,
  vertical). The rest surface as one-key confirmations near the cursor while
  drawing — the transient strip of the action surface.
- Committed inferences are shown at commit and declinable in one action;
  the decline is finer-grained than undo (undo removes the placement plus its
  inferences as one step; decline removes one inference).
- Preview shows truth: the rubber-band during drawing runs the same inference
  that commit will run, ghost glyphs included. What you see mid-gesture is
  what you get.
- Grid snap stays a placement aid, not a constraint generator; a document
  where every point is grid-pinned is rigidity by helpfulness.
- Ranking among simultaneous candidates is contextual and document-local
  (recent choices in this document weigh in), deterministic and inspectable.
  No global learned magic.

No invisible constraints, ever: every constraint is reachable from the
geometry it binds — hover reveals, selection walks, and each has a glyph.
Glyph overload is real at scale, so glyph visibility is a computed per-frame
set (related to selection, hover, recency, zoom-dependent density budget) —
a policy over the whole overlay, not per-glyph booleans.

### Screen-calibrated gestures, absolute numbers

Hit radii, snap radii, drag thresholds, inference tolerances are pixel
quantities, converted through the view transform per query; consequently the
same sloppy gesture infers coarser relations zoomed out than zoomed in, which
matches user intent at each zoom. Typed values, stored geometry and units are
document-absolute and zoom-independent. Every zoom bug is a leak between
these two regimes, so the split is architectural: one side owns pixels, the
other owns millimeters, and conversions happen at named boundaries.

### Modes and selection depth

Selection is the home state; Esc always lands there. Creation tools are
verb-noun by necessity (the noun does not exist yet) and stay shallow: no
nested modes, live tool parameters in a fixed strip, Esc out. Everything else
— constraints, styling, arrangement — is noun-verb over the current
selection, because select-then-act is how artists think and how the
signature-driven UI works.

There is no edit-mode wall. Object versus component is selection depth, not
mode: double-click descends (shape to its edges and points, along the
coincidence graph for connected runs), Esc ascends, and mixed-depth
selections ({this rectangle, that one vertex}) are legal and produce honest
signatures. Constraints are themselves selectable through their glyphs —
deletion and editing of a relation uses the same selection machinery as
geometry, which is also what makes conflict sets walkable.

## Transforms against constraints

Vector users expect free transform of anything; parametric documents push
back through two specific frictions, both addressed by modeling rather than
by warning dialogs.

Rotation versus axis constraints: horizontal and vertical are not intrinsic
properties — they are parallelism to a reference axis, and the model records
them that way (the document frame is the default reference). Rotating a
subset that carries axis-referenced constraints is then a real question with
two honest answers — retarget the constraints to a rotated frame (the
subset's "horizontal" tilts with it, rigid-body style) or keep the document
axes and let the solver fight the rotation. The action surface asks, once,
with preview. A per-cluster frame is just construction geometry, so this
costs no new machinery.

Scale versus fixed dimensions: relational constraints (parallel,
perpendicular, angle, ratio, equal, midpoint, symmetric, tangent) are
scale-invariant; absolute ones (distance, radius, length difference) are not.
This asymmetry is the thesis vindicated: a proportion-built document rescales
cleanly, absolute dimensions pin physical size, and the two families should
be visually distinguishable. Scaling a subset containing absolutes offers
scale-the-values (rewrite the slots by the factor — trivially expressible
because values are slots) or let-them-resist. Non-uniform scale of
constrained geometry is refused honestly (it does not commute with almost any
constraint); it remains available as a bake at export.

A third case surfaced in practice and is narrower than both questions: the
origin-referenced symmetric forms (symmetric-horizontal, symmetric-vertical)
mean symmetry about the document frame through no operand, so neither answer
applies — there is no slot to rewrite and no operand to retarget. Transforms
refuse a selection carrying one, and copies drop the relation with a count,
because the frame sits outside every copied set. The about-line form, whose
axis is an ordinary operand, stays fully relational and travels like
everything else; the taxonomy marks the frame-referenced pair so no walk has
to know the kinds by name.

## The action surface

Actions are data. One registry holds every action: its applicability
predicate over (signature, document state), its parameter schema, its
bindings. Menus, the context strip, the command palette, keyboard dispatch
and the scripting-and-test harness are projections of the registry, so an
action reachable one way is reachable every way, and an action inapplicable
in the model is offerable by no surface. Applicability shares the same tables
the solver-side validation uses — a single source of truth for "what can
apply to {segment, segment}" — which is what keeps the UI honest for free as
the catalogue grows.

Surface discipline balances discoverability against muscle memory: the
persistent surfaces are spatially stable — slots do not reshuffle with
context; inapplicable actions dim rather than vanish — while context
sensitivity arrives additively through a transient strip near the work
(post-placement inference confirms, signature-ranked constraint offers,
role prompts). Ranking within the strip is contextual; placement of the
permanent furniture is not.

Role ambiguity resolves in the surface, not in prose: point-to-line distance
with {point, segment} auto-assigns roles; length-ratio over {segment,
segment} asks which way, with preview. Speculative solves (below) let any
offered constraint be previewed on hover — the geometry ghosts to where
commit would put it, which turns the catalogue into something learnable by
looking rather than by reading.

Every action is invocable headlessly with parameters, because the registry is
the automation surface. The selftest philosophy extends: scripted sessions
drive the same code paths the pointer does, and feel invariants (below) run
on them in CI. Keyboard reachability of every action falls out of the same
property.

## No silent changes

One policy, many surfaces, single statement: nothing the system does on its
own initiative is invisible. Solver ripple beyond the viewport pings the
screen edge. Hidden geometry influencing a solve is indicated. Inferred
constraints announce themselves and stay declinable. Deletion reports what it
took with it (counts, not confirmation dialogs). Regions and tags degrade
visibly, never evaporate. Imposition that does move geometry (the parallel
snap-shut case) shows the motion. Automatic behavior is trustworthy exactly
insofar as it is observable; this policy is why inference and solver
mediation can be aggressive without the tool feeling haunted.

## Undo, deletion, copy

Undo is command-sourced over declarations, and every undo record carries the
seeds of affected components, because replaying declarations without seeds
can legally produce a different branch than the one the user was shown. Undo
restores what was seen, not merely what was meant. Inference bundles with its
gesture: place-with-snaps is one undo step; declining one inference is its
own finer step.

Deletion has two levels by design. Relations referencing a deleted operand
die with it — a constraint without its operand is meaningless — and the
removal is counted visibly. Higher-order dependents (regions, tags) degrade
per the equivalence sections and never block. There is no "cannot delete:
in use" anywhere in the tool; that dialog is how other tools export their
bookkeeping problems to the user.

Copy takes internal structure: constraints with both operands in the copied
set come along (with fresh IDs throughout); boundary constraints — one
operand outside — are dropped and the drop is indicated, with re-binding
offers reserved for the transient strip. Duplicate-with-offset preserving
internal structure is deliberately the seed of future array and pattern
features rather than a separate system.

## Engineering the feel

Feel is a latency-and-stability budget, engineered like one:

- The interactive loop — input, solve, raster, present — fits the frame
  budget. The solver is the variable term, so solves are scoped to the
  dragged component, warm-started from current seeds (Newton from a
  near-solution converges in a step or two), and measured continuously.
- When a component exceeds budget anyway, that component's solve degrades to
  asynchronous with a generation counter: the UI never blocks, stale results
  are discarded by generation, the last coherent solution renders throughout,
  and partial solutions are never blended. Synchronous-under-budget is the
  norm because asynchrony has a feel cost (rubber-banding) that predictable
  small systems should never pay.
- Speculative solve contexts are first-class: previewing a hovered
  constraint, testing a candidate for consistency at creation, evaluating a
  scrub — all fork a component's parameters, solve the copy, and throw it
  away. The document is immutable during preview. This requires the solve
  context to be a value type, cheap to clone — a representation decision that
  must hold from the first document model, since previews, async solving,
  creation-time validation and animation evaluation all sit on it.
- The component partition is maintained incrementally as the constraint graph
  edits (union on connect; deletions mark for periodic rebuild). Its quality
  is a leverage point for everything above, and it is also the natural
  boundary for parallel solves of independent components.
- Warm-start continuity doubles as branch stability: consecutive solves along
  a drag or a parameter sweep stay on the same solution branch because each
  starts from the last. A branch flip mid-gesture is a bug by definition,
  reproducible from recorded seeds, testable.

When iterative discovery lands on a behavior that feels right, it is frozen
as a measurable invariant so it cannot regress silently: drag locality as a
displacement bound outside the dragged component; branch stability as
sign-invariants across scripted sweeps; inference quality as precision and
recall over a corpus of recorded gestures; budget compliance as solve-time
percentiles per component size. Feel is subjective at discovery time and
objective forever after.

## Substrate boundaries

Seams, in dependency order, each crossable in one direction only:

- Document model: entities, constraints, slots, regions, roles, tags,
  topology, IDs. Depends on nothing below. No Qt types, no Skia types, no
  slvs types — solver handles do not leak upward.
- Solve layer: translates a component to an `Slvs_System`, runs it, maps
  results back to document IDs. Owns seeds-in-flight, speculative contexts,
  generations, the arena for each solve. The only file that includes
  `slvs.h`.
- Raster and hit layer: consumes solved geometry; owns tessellation, the
  spatial index, the adorner overlay. The only layer that includes Skia.
- Toolkit shell: `sketchview` stays the single Qt-aware seam, per the
  existing layout. The renderer sits behind a paint-document-plus-overlay
  interface so the known QQuickPaintedItem shortcut can be swapped for the
  Ganesh/RHI path without anything above the raster layer noticing.

The current `sketch.cpp` deliberately mixes solve and raster to prove the
toolchain; the document-model milestone splits it along the seams above.

Space discipline: geometry in document space, adorners in screen space,
always. Handles and dimension text do not scale with zoom; hit tolerances are
pixels inverse-transformed per query (the Eigen view transform already exists
for exactly this dual use). Hit-testing is not rendering: it has its own
spatial index over solved geometry and one replaceable priority policy
(points over edges, selected over unselected, construction demoted, adorners
over geometry) — a policy function, because hover priority is a feel item
that will be iterated.

Concurrency and memory: the document has a single writer on the UI thread.
Workers (async solves, speculative previews, later animation evaluation)
receive immutable component snapshots and return messages tagged with
generations; nothing shares mutable state, so our code needs no locks on the
interaction path — Qt's internals are outside this rule. Each solve runs out
of an arena (mimalloc heaps; the vendored solver already allocates its
temporaries through `mi_heap` in `platformbase.cpp`), so a solve's memory
lifetime is the solve. Solver isolation stays absolute: the C API boundary
holds standards, allocators and exceptions apart today, keeps a future
homegrown 2D solver (and the licensing exit the README sketches) possible
tomorrow, and is the line behind which solver replacement strategies live if
scale demands them.

Persistence: the file is the declaration layer plus seeds plus roles and
tags — authoring intent and branch choice, nothing rebuildable (no
tessellations, no indexes). Text-based, stably ordered so diffs are readable
and merges are sane, versioned from the first write, and unknown record kinds
survive round-trips (a newer file opened in an older build must not shed the
parts the old build does not understand). Export to SVG and friends is a
bake: solved geometry out, constraints dead, region algebra flattened to
masks — labeled as such. Import is tracing: geometry arrives free and
unconstrained, and inference-on-import is a later feature with the same
taxonomy, not a second inference system.

## Animation-readiness

Animation is parameters over time: keyframe the slots, solve per sample, and
relationships hold while values travel — spacing interpolates, attachment
persists, which is animation of intent rather than of coordinates and is the
payoff of the parametric core. Nothing of the timeline UI is built now, but
three commitments are cheap today and prohibitive later: every value is a
slot (a keyframe track is one more slot kind); solve continuity and recorded
seeds make sweeps deterministic and pops (branch flips) impossible by
construction, with per-sample seed caching for random access scrubbing; and
solved samples are serializable values, which the snapshot-based solve
contexts already require. The measurements-never-drive rule matters doubly
here — a measurement-driven keyframe would couple sample N's output into
sample N+1's input and make scrubbing order-dependent.

## The v1 catalogue

Primitives: point, line segment, 3-point arc (mapped onto the solver's
center-form arc plus an on-circle constraint for the through-point), circle.
The solver's entity vocabulary (line, arc, circle, cubic) bounds what is
cheap: cubics are the door through which beziers later enter as ordinary
primitives with their own constraints (`SLVS_C_CUBIC_LINE_TANGENT`,
`SLVS_C_CURVE_CURVE_TANGENT` already exist), while ellipses and offset curves
have no solver entity and are therefore expensive propositions requiring
explicit justification, not casual additions.

A primitive is admitted only with its full relation story: creation tool,
snap candidates, applicable constraints, degradation behavior, export
mapping. A primitive without the story is a feature that will strand its
users at the first constraint they reach for.

Constraints, curated for the layout domain from what `slvs.h` offers:
coincident, point-on-line, point-on-circle, midpoint, horizontal and vertical
(recorded as axis-referenced), parallel, perpendicular, angle, equal angle,
point-point distance, point-line distance, equal length, length ratio, length
difference, symmetric (horizontal, vertical, about-line), tangent (arc-line
first), radius, equal radius, pin (`WHERE_DRAGGED`). Ratio and the
equal-spacing chain deserve emphasis: they are the thesis constraints —
proportion and rhythm as declared intent — and the demo's driven
len(A)/len(B) is the pattern in miniature. Compound relations (distribute,
mirror-group, spacing chains) are macro-expansions with tags per the shape
rule; the solver never learns about them.

## Scope guards

Fences, so the base stays buildable and the equivalence principle stays
bounded:

- Unify representation identities only. No speculative generality: an
  equivalence is honored when its math is an identity and its usage is
  obvious, and discovered cases queue behind the same test. Translation
  layers between live representations are refused categorically.
- v1 non-goals: bezier editing, text, raster images, 3D, a general
  expression language (slots hold constants and minimal arithmetic with
  named parameters), plugin and scripting APIs (the registry exists but is
  not yet public surface), constraint DSLs, Windows packaging, collaborative
  editing. Each is deferred, none is precluded — slots, tags, the registry
  and the C-API seam are the hooks they will hang from.
- Scale hypothesis, stated so it can fail loudly: real documents are many
  small components, not one giant rigid mesh; the partition keeps interactive
  solves in the tens-to-hundreds of parameters where SolveSpace is
  comfortable. Component-size percentiles are benchmarked; if the hypothesis
  breaks, the responses live behind the solve seam (decomposition
  strategies, solver replacement), never above it.
- Feel items parked for iterative discovery, so their absence here is
  legible: hover priority weights, inference ranking and thresholds, glyph
  density budgets, saturation feedback strength, async degradation
  thresholds, dissolve-versus-diagnose defaults for broken regions and tags.
  Each lands as a replaceable policy behind a stable interface, then gets
  frozen into invariants as discovery settles.

## Load-bearing interactions

The couplings that make these principles a system rather than a list — the
implementation plan must respect these as ordering constraints:

- The taxonomy spine: primitives, constraints, snap candidates, applicability
  predicates and action metadata share one schema. Adding a primitive or
  relation is a data change that updates drawing, snapping, the action
  surface, validation and the test harness together — or it is five drifting
  copies. This is the deepest structural bet after the solver itself.
- The seeds thread: branch selection, undo fidelity, scrub determinism and
  file-reopen fidelity are one decision — persist seeds — surfacing four
  times.
- The slot thread: typed input, scrubbing, dimensions, scale-the-values,
  expressions and keyframes all hang off value-cells-as-slots. One
  indirection, six features.
- The snapshot thread: warm-started drags, speculative previews,
  creation-time validation, async solving and animation evaluation are the
  same capability — solve contexts as cheap value types — exercised five
  ways.
- The topology thread: the coincidence graph underlies regions,
  heal-and-fill, selection expansion and the component partition. One graph,
  maintained incrementally, four consumers.
- The registry thread: applicability truth, every UI surface, keyboard
  dispatch, headless testing and future scripting are projections of one
  action table.
- The trust policy: saturation attribution, ripple pings, inference
  visibility, deletion counts and degradation states are five faces of
  no-silent-changes; weakening one weakens the product's core claim, which
  is that the user can hand intent to a solver and never be surprised.
