# Review: stages 0 through 8

A full correctness pass over the tree at commit 3b19e9f (2026-07-21), before the
production-UI work begins. Eight subsystem reviews — core model, persistence and
format, solve and async, session core, registry and scripts, structure
operations and boundaries, render and shell, tests and build — followed by an
adversarial verification pass in which every finding below was independently
re-traced and, where possible, refuted. Findings that did not survive are gone
or carry their correction; mechanisms that could not be fully confirmed are
listed separately as suspicions. `nix flake check` is green on this tree, so
everything here is latent: severity means cost-if-unfixed once a UI is built on
top, not brokenness today.

Two patterns recur and are worth naming before the list.

First, constraints carry implicit references that no operand walk can see: an
arc's start/end are index labels that mirror permutes, an unreferenced
horizontal means the document frame, and the symmetric-horizontal pair means
the world origin. Copy's straddle test and the transform frictions reason over
operands only, so each of these survives an operation that changed what it
means. Findings 1 through 3 are one gap seen from three sides, and the stage 7
lesson — defects cluster where a mechanism is inferred rather than shown —
holds for a third stage running.

Second, every interaction finding was invisible to a green suite because the
tests construct their inputs above the seam where the bug lives: the corpus
builds KeyStroke records by hand so the shell's translation is never exercised,
the applicability sweep never locks a layer, the numeric tests always commit by
Enter, and the async tests never edit between submit and drain. The gap list at
the end is as much the deliverable as the findings.

## Disposition

A fix pass landed on this same tree, verified by a second adversarial review
wave over the finished diff, with the full suite and `nix flake check` green.
Findings 1 through 13, 15, and 17 through 25 are fixed, each carrying a
regression test except where noted. The particulars that go beyond a plain fix:

- Finding 3 closed both halves. Copy drops and counts the frame-referenced
  kinds through a new taxonomy column; rotate and scale refuse whole with
  TransformError::FrameReferenced — retarget is impossible for a relation whose
  frame is not an operand — and the registry rows dim to match, so applicable
  still equals runnable. The review wave forced the transform half: the first
  cut left the kinds to resist, which is exactly the silent slide-back the
  finding describes.
- Finding 2's drop counts reach the surface: the review wave found mirrorStep
  discarding CopyStep's counts, so CompoundStep now carries them to the
  structure report the way duplicate's already did.
- Finding 12 is fixed for X11 and Wayland by deriving the engraved digit from
  the native scan code; a platform reporting scan code 0 falls back to the
  keysym range and shifted digits stay dead there. The shell's event
  translation remains untestable in the harness — the tests target links no
  Qt — and that gap stands.
- Finding 14 landed as: IME flag dropped, and a stroke carrying Control now
  resolves to nothing across every reading in resolveKey — offers, fields and
  commands — so no platform's text() encoding can alias a Ctrl chord into a
  bare command. The ctrl+ binding grammar itself stays with the production-UI
  stage.
- Finding 16 is by design and is now documented as pumpAsync's integration
  contract in session.h: refresh() alone is one epoch behind, and the UI must
  pump on a timer or idle hook.
- Finding 21's spec half closed in FORMAT.md: version-0 additive changes are
  restricted to new record kinds, since only unknown kinds survive an old
  reader.
- Finding 11's reset was extended to selectConflicting during the review wave —
  the walk-conflicts flow was the one selection change the first cut missed.
- Finding 24's test was restructured after the review wave showed the first
  version's pin was timing-dependent; the stale solve is now held until the
  fresh result has landed, so the assertion catches a removed epoch check
  regardless of batch order.

Of the minor notes, fixed: the stripEntries context rebuild, the device-size
double rounding, the punch-free saveLayer, the broken-region fallthrough, the
mode-flag precedence, the refresh-skip exemption comments, the allocator
ceiling asserts, the null-id error message, the arc tessellation abs, and
non-finite SVG coordinates (including the defaulted rect/circle attributes the
review wave caught coercing to zero). Left open deliberately, as accepted
notes: the allocator guards are debug asserts, so a release build loading a
file carrying the maximum id still wraps; the compound id arithmetic is
unguarded the same way; scheduler applied_ grows per distinct component key;
canImpose accepts degenerate geometry a candidateFor then refuses; the script
format's end-document delimiter coupling; non-finite seeds round-trip bytewise
while breaking record equality; and the CLI has only the four registered
smoke tests. The suspicions section below is unchanged and still worth a
future pass. Two new notes from the review wave: finding 4's diamond test
guards rather than pins (the depth-60 chain is the pin), and finding 5's
footprint is exact for circles and radius-only arc kinds but over-inclusive
for an arc under point-on-circle and tangency — non-regressive, the entity
granularity it replaced was equally coarse.

One reconciliation for CLAUDE.md when it is next edited: "every relation in
the catalogue is translation-invariant" is no longer the whole story — the
frame-referenced kinds are the counterexample, now handled by refusal in
transforms and drop-and-count in copy.

## Structure operations

1. Mirror mislabels a tangency's end on a reflected arc. Major.
copy.cpp:85-87 swaps a reflected arc's endpoints, correctly, because the
un-swapped copy is the complementary arc. But copy.cpp:114 copies every
constraint verbatim, and a tangency selects its touch point by index —
measure.cpp:309 reads `points[alternative == 0 ? 1 : 2]`, and the solver reads
`other ? 2 : 1` identically (solve.cpp:285, constrainteq.cpp:942). After the
swap the copied tangency names the wrong physical end, for either value of
`alternative`. Mirroring an arc with its tangent line — an ordinary
fillet-mirror — yields a cluster that is over-constrained (flagged and frozen
at correct-looking seeds) or, with slack, visibly distorted. Angle and
equal-angle's alternatives are unaffected: they flip a segment direction, and
mirror swaps no segment ends. Fix: in copyStep, toggle `alternative` on a
tangency whose arc received the endpoint swap.

2. Mirror carries axis constraints into contradiction with its own symmetry.
Major. mirrorStep (compound.cpp:286-324) adds a SymmetricAboutLine per copied
point, pinning the copy to the reflected pose, but unlike rotateStep it never
asks the axis question. A horizontal with no reference names the document frame
through a null operand, boundOperandCount is 1, and copy's operand-only
straddle test (copy.cpp:98-116) calls it internal and copies it verbatim. About
an axis at angle θ the image of a horizontal edge lies at 2θ, so for θ outside
{0°, 90°} the copied horizontal and the symmetry contradict: mirroring a
rectangle about a diagonal guide produces a component flagged Inconsistent and
frozen — original included — over seeds that look right. Fix: mirror needs the
same treatment rotate has — retarget the copied axis constraints to a reflected
frame, or drop them with a count.

3. The origin-referenced symmetric kinds break silently under copy and
transform. Major. SymmetricHorizontal/Vertical constrain about the workplane
axis through the origin (measure.cpp:273-284; solver `au+bu=0`,
constrainteq.cpp:796-802) — an absolute reference carried by no operand. The
taxonomy marks them ScaleInvariant with no column for translation- or
rotation-variance, so they fall through both transform frictions
(transform.cpp:160, 177-179) and through copy's straddle test. They are
imposable from the strip over any two points, and being valueless the
imposition moves geometry to satisfy them — so a satisfied pair is easy to
author. Duplicate-with-offset then copies the relation verbatim and the copy's
component is consistent on its own, so the solver silently slides the pair back
toward the world axis by the offset; rotate and off-origin scale are broken the
same way, with no conflict flag. The transform.cpp claim that every catalogue
relation is translation-invariant is false for exactly these two kinds, and
translateStep (transform.cpp:402) is dead code — no caller. Fix: mark the two
kinds position-referenced in the taxonomy and have copy drop-and-count them
(and transforms refuse or retarget), or give them an explicit axis operand.

## Core model

4. Parameter evaluation is exponential in the reference graph. Major, file-only.
TableParameterEnv::lookup (parameters.cpp:8-14) re-evaluates the referenced
parameter's whole slot with a fresh env; the per-slot forward pass memoizes
within one slot only, so a slot referencing the same parameter through two
nodes doubles the work per level. Verified empirically against this code:
2× per level exactly, 35 s at depth 30, and the depth-64 bound caps recursion,
not work. wouldCycle memoizes its walk, so building the chain is milliseconds
per add — the trap is invisible until first evaluation, which sits on the solve
(solve.cpp:318), the frame (view.cpp:244, 315, 569), the bake, and parameter
deletion. No command surface creates parameters today, so this is a
denial-of-service through a crafted file — in scope for a frozen format whose
loader treats files as untrusted, and invisible to the fuzz suite because it is
a timing failure, not a round-trip one. Fix: memoize resolved parameter values
across one top-level evaluation, exactly as wouldCycle already does its walk.

5. A lock can make a system inconsistent through a partially frozen circle.
Minor, mechanism confirmed. allFrozen (solve.cpp:132-139) is entity-granular:
a circle on a locked layer whose centre point sits on an unlocked layer has
isFrozen(circle) false (composition.cpp:138-144), so its Radius constraint is
emitted — while groupFor, which is layer-only, puts the radius parameter in
GROUP_BASE as a known. A dimensioned circle's seed generally differs from the
driving value, so the component reports Inconsistent, and the lock caused it —
the one thing a lock must never do. Reachable only by deliberate layer surgery
(move the centre alone to another layer), and recoverable by unlocking. Fix:
base the omission on whether the constraint has any unknown among the
parameters it actually binds, not on whole-entity frozenness.

6. Entity and region deletion steps do not compose. Minor, latent.
document.cpp has no overload taking entities and regions together;
deletionStep(RegionId) emits Remove(region) while the entity overload emits
SetRecord shrinks of the same region, each computed against the unmodified
document, so one concatenation order applies Remove then Set and the whole
gesture rolls back to a silent no-op. Dormant: deleteSelection feeds only the
entity+constraint overload, regions are never in the doomed set, and nothing
reachable today concatenates the two. The production UI's "delete this fill and
one of its edges" is the obvious first caller. Fix: a combined overload
computing all shrinks and removals over the whole doomed set once.

## Interaction

7. A typed value survives a click commit and corrupts what follows. Major.
Committing a placement by Enter cancels the numeric entry
(session.cpp:515); committing the same placement by pointer press does not —
the Press branch, commitPlacement and runTool contain no cancel — and
refreshToolPresentation republishes the still-active field. resolveKey
(registry.cpp:571-575) routes every printable key into an active field, and the
entry grammar accepts letters for units (numeric.cpp:21-24). So after
type-then-click: command keys are swallowed into the field until Esc, and worse,
the stale text prefixes the next placement's value — type 5, click, type 3 for
the next segment, Enter commits at 53. Silent wrong geometry on a natural
gesture; no test commits a placement by click while a value is typed. Fix:
cancel the numeric entry when a pointer press commits or opens a placement.

8. Undo, Redo and Delete are unguarded during a gesture. Minor. Escape ascends
carefully through numeric, drag and tool (session.cpp:924-964); Delete, Undo
and Redo dispatch unconditionally, and keys are delivered mid-drag. Verified
outcomes: the corruption path is closed — commitCommands skips deleted
entities and applyStep is atomic — but undo mid-chain leaves a line tool's
anchor dangling so every subsequent click is refused until Esc, and a mid-drag
undo yields a surprising-but-valid commit that truncates the redo tail.
Recoverable UX defects, not corruption. Fix: have these keys cancel or refuse
while a gesture is in flight, mirroring Escape.

9. A press on a segment body falls through to a selection-clobbering marquee.
Minor. A segment is hittable but owns no parameters, so beginDrag returns
nothing (drag.cpp:114-121), and the Move handler's fallback
(session.cpp:806-815) starts a marquee from the press point — there is no
pressed-entity guard — whose release replaces the just-selected connected run
with whatever the box caught, usually nothing. Grab-the-edge-and-move both
fails to move and clears the selection. Fix: a failed beginDrag on a pressed
entity should keep the selection rather than fall through to the marquee.

10. Rotate and scale claim applicability over a locked selection and then
refuse. Moderate. haveTransformable (registry.cpp:257, 646-649) counts
transformable entities with no lock check; rotateStep/scaleStep refuse whole on
any lock, applyTransform returns false, and the palette enables the rows
(Main.qml:168). The refusal does reach the status readout
(sketchview.cpp:454-457), so it is inconsistent rather than silent — but it is
exactly the applicable-and-refusing the registry's own comment
(registry.cpp:259-267) says would break the table's trustworthiness, and the
non-uniform-scale row was dimmed to avoid. Duplicate correctly runs over a lock
(copies are new geometry). The applicability sweep never locks anything, so
applicable-equals-runnable is untested under lock. Fix: the predicate mirrors
the transform's own gate and the rows dim.

11. A refused imposition's downgrade offer outlives the selection it names.
Minor. presentation_.downgrade is documented "cleared with the selection"
(session.h:173) and select() clears it, but the pointer selection path mutates
selection_ directly and refreshSelectionOffers never resets it; Esc and
deleteSelection leave it too. The strip renders the stale offer unconditionally
(surface.cpp:123-135). Bounded consequence — invoking it re-checks
applicability and at worst imposes an unsolicited reference measurement — but
it is a visible contract violation on the common path. Fix: reset it wherever
the selection changes.

## Shell input

12. The digit field is read from the resolved keysym, so decline is dead on a
US layout. Major. strokeOf (sketchview.cpp:273-275) sets `digit` only for
event->key() in Key_1..Key_9, but registry.h:184-188 defines digit as the
engraved key independent of shift and layout. Shift+1 arrives as Key_Exclam,
digit stays 0, and resolveKey's confirm/decline block (registry.cpp:554) is
skipped; '!' then falls to the letter path and is rejected. Decline has no
other surface — the strip does not carry it, and the palette row refuses for
lack of its index parameter — so shift+1..9 is simply dead on the most common
layout, and on AZERTY the confirm side breaks too (unshifted digits deliver
symbol keysyms). The unit tests pre-set `digit` on hand-built strokes and the
corpus records confirms by index, so nothing exercises strokeOf. Fix: derive
the engraved digit from the physical key, and put one test through the shell
translation.

13. A middle-button pan swallows a left release and the stranded drag commits
later. Major. While panning_, mouseReleaseEvent forwards nothing and only a
middle release clears the flag (sketchview.cpp:219-223); mouseMoveEvent is
likewise swallowed. Press left on an entity, press middle, release left before
middle: the session's press state and drag survive with no release ever
delivered. With hover events enabled, subsequent bare-cursor motion enters the
still-armed Move path — a drag tracks or even begins with no button down — and
the next innocent left click's release commits the stray move as a real edit
(session.cpp:899-900, 1264-1271). Fix: track buttons independently; forward
left transitions to the session even while panning.

14. Minor input items, bundled. The item sets ItemAcceptsInputMethod with no
inputMethodEvent handler (sketchview.cpp:27), inviting an active IME to eat
digits and tool letters as preedit; drop the flag or forward committed text.
The binding grammar cannot spell a Control or Alt chord — resolveKey consults
only Shift (registry.cpp:591-598) — so the coming UI's Ctrl+Z/C/V cannot be
registry projections; the feared Ctrl+letter aliasing does not occur on X11
(the control character is rejected by the letter guard), and Alt+letter is a
benign synonym today. No autorepeat guard exists, but held Enter and Delete
verify as self-gating no-ops; note only.

## The async seam

Async is opt-in and nothing in src/shell or src/app calls enableAsyncSolving or
pumpAsync yet, so nothing here is user-visible today. All three items are the
bill for wiring it into the UI, presented in advance.

15. Async components vanish from the readouts. Minor now, blocking then.
refresh() accumulates dof, worst-status and hidden-influence only on the
synchronous branch (session.cpp:207-217); applyAsyncResults never reads
outcome.dof, and its status escalation is overwritten by the sync-only recompute
at session.cpp:253 when draining inside refresh. A fully-async consistent
document reads dof −1 — rendered "unsolved" — and an Inconsistent async
component can read Okay. The pose itself is always correct; only the readouts
lie, and PRINCIPLES treats the dof display as load-bearing calm. Fix: fold each
applied result's dof and status into the presentation, and compute
hidden-influence for async components.

16. Threaded results land only through pumpAsync, which nothing calls.
Integration trap. refresh() bumps the epoch before draining, so anything a
worker finished under the prior epoch is discarded at the tag check and
resubmitted — refresh alone is perpetually one epoch behind, by design. The UI
must pump on a timer or idle hook and must not bump the epoch doing so; the
only current caller is the test. Document this on the wiring task or the
symptom will be permanently frozen poses. Related notes: scheduler applied_
grows one entry per distinct component key forever (scheduler.cpp:140), and the
epoch-mismatch drop branch has no test (see gaps).

## Persistence and interchange

17. SVG import ignores transforms and counts the result as traced. Major
within its scope. No code in svg.cpp reads a transform attribute — the g
element is a structural no-op and tracers read only their coordinate
attributes — while svg.h:61-62 documents transforms as "skipped and counted."
A translated group traces at untranslated coordinates and increments traced_,
so ubiquitous real-world SVG imports wrong geometry reported as success,
breaking the loss report's own contract. Import is a trace and may be lossy;
it may not be silently wrong. Fix: detect a transform on the element or any
ancestor and skip-and-count the element whole (or apply affine transforms).

18. The version gate wraps for versions at or above 2^31. Minor. persist.cpp:490
compares `static_cast<int>(*version) > FORMAT_VERSION` on a full-range uint32,
so 3000000000 casts negative and loads as version 0 instead of refusing,
against FORMAT.md's refuse-whole rule. Every realistic version (1..2^31−1)
refuses correctly, so this is corruption-hardening; script.cpp's version parse
is signed and unaffected. Fix: compare unsigned.

19. Malformed references and colours coerce to zero instead of refusing.
Minor. Six fields — entity and region layer= and style=, style stroke= and
fill= — parse via value_or(0) (persist.cpp:553-554, 595-596, 681-682) while
order= and z= refuse. A corrupt stroke colour becomes fully transparent
geometry with no error; layer=main silently lands on the base layer, and the
null id skips the dangling-reference validation that a wrong numeric id would
trip. Reachable only through corrupt or hand-edited files. Fix: refuse like
order= does.

20. An under-length seed list zero-fills. Minor. The seed parse refuses only
provided > ownParamCount (persist.cpp:612-616) where the point list refuses
any mismatch; missing seeds stay 0.0 and re-save normalizes the record,
against FORMAT.md's "exactly its own parameter count." Fix: refuse both
directions, as the adjacent comment already claims.

21. Additive fields do not survive an old reader, and the export write is
unchecked. Minor, two items. FORMAT.md's migration policy is internally
consistent — it says unknown fields are skipped — but the consequence is that
its "new optional field on an existing kind" additive path is lossy through an
old reader's round-trip, unlike a new record kind, and the future.paro corpus
only tests the kind case; either restrict the policy to new kinds or preserve
unrecognized tokens. Separately, --export checks only that the stream opened
(main.cpp:38-45): a disk-full short write still prints success and exits 0.

## Test and build gates

22. The arena-bytes bench gate never runs. Major as a gate. paroculus-bench
has no add_test — only unit, selftest and the two canaries are registered —
so the one deterministic regression gate stage 8 added executes only by hand,
and its baseline path is CWD-relative with a silent-success fallback when
missing (bench/main.cpp:34, 207-211). Fix: a ctest entry running an
arena-only check mode with the baseline path compiled in.

23. The boundary canaries guard one seam of four. Major as a gate. Both
canaries link paroculus-core only (CMakeLists.txt:370), which proves core
cannot see slvs.h or Skia — but the realistic regression, a PRIVATE link
flipped to PUBLIC on paroculus-solve or render leaking headers into interact
and shell, leaves core untouched and both canaries still failing to compile,
so the WILL_FAIL tests stay green. No canary covers interact-must-not-reach-Qt
at all. Fix: canaries that link paroculus-interact and reach for slvs.h and a
Qt header.

24. The async epoch drop is untested. The r.tag != docEpoch_ branch — the one
that prevents applying a solve of a document the user has since edited, which
per-component generations cannot catch — is exercised by nothing:
tests/gestures/async.cpp never edits between refresh and pump. A test that
holds a solve in the hook, applies a partition-changing edit, releases, and
asserts the drop.

25. The locale round-trip test cannot fail in the sandbox. The comma-decimal
test falls back to a substring check when no European locale exists
(core_persist.cpp:507-524), and the flake ships none — so the printf-regression
it guards would pass nix flake check. Add glibcLocales to the test derivation
or REQUIRE the locale was exercised.

## Minor notes

- stripEntries rebuilds the full ActionContext four times per call
  (surface.cpp:98, 110-113) though its four predicates read only cheap flags;
  paletteEntries already hoists it. Worst case O(N²) ×4 per strip build, on a
  path the QML property getter re-invokes per refresh.
- canImpose accepts degenerate geometry a candidateFor then refuses
  (zero-length segment under length-ratio) — a narrower applicable-then-refuse
  of the finding-10 shape.
- The texture and paint surface round device size differently
  (sketchview.cpp:126 vs 678-682), off by one pixel at fractional logical
  sizes; compute the device size once.
- drawFills opens a full-canvas saveLayer per fill-bearing layer whether or not
  anything punches (view.cpp:598-657); gate it on a punch being present.
- A Whole region whose Skia path op fails draws neither fill nor diagnostic
  (view.cpp:641-642); fall through to the broken diagnostic.
- Mode flags beyond --script/--record have silent precedence (--export beats
  --import beats --selftest) with no diagnostic (main.cpp:102-137).
- newLayer, groupSelection and dissolveGroups skip refresh(); verified harmless
  today (no solve input changes, and bumping the epoch would needlessly drop
  in-flight async results), but document the exemption where moveLayer
  documents the precedent.
- IdAllocator wraps to the null id after 2^32 allocations with no guard
  (ids.h:74); compound.cpp:293 arithmetic can overflow the same way. Assert at
  the ceiling.
- Non-finite seeds round-trip bytewise but NaN breaks Document equality, and
  SVG import accepts nan/inf coordinates where export folds them to 0.
- persist refuses id 0 with a misleading "duplicate id" message, and an
  explicit trailing null operand is normalized rather than refused.
- The script format's embedded document ends at a literal end-document line —
  safe today because persist never emits one at line start, but an undefended
  delimiter coupling.
- The app CLI dispatch (--export/--import/--script argument handling, exit
  codes) has no automated test; only --selftest is registered with CTest.

## Suspicions, not confirmed

Each with what would settle it.

- The solver-serialization gate assumes every synchronous solve caller is the
  UI thread that constructed the scheduler; a future solve from an unrelated
  thread could read sharing==0 unfenced and race a worker inside Slvs_Solve.
  Audit call sites when any second thread appears.
- An allocation failure deep in SYS.Solve would unwind through the extern-C
  seam uncaught and terminate a worker. Trace SolveSpace's solve-path
  allocations or wrap the call.
- bakeForExport emits a fill for every Whole region regardless of the style's
  filled flag (bake.cpp:85-86); if render gates on it, exports show fills the
  screen does not. Compare against the render path.
- After the first fit latches, a window resize recomposes around a stale
  centred base (view.cpp:445-458) and content drifts from centre; decide
  deliberately whether that is the intended reading of "the framing belongs to
  the user".
- distribute's degeneracy guard checks the span's ends only; two coincident
  interior points yield a zero-length gap segment plus an equal-length over it.
- transformClosure is downward-only, so a points-only selection misses its
  segment's axis constraints under rotate-with-retarget; unreachable through
  normal selection, which includes the segment.
- The retarget frame takes the first moved entity's layer; if that layer is
  locked while others are not, the frame's pins freeze and the frame goes
  free. Obscure cross-layer configuration.
- An arc opened on an existing vertex with the bulge then reversed: the
  pending-snap zip against out.opened under reversal is what tools.cpp:441
  itself flags, and nothing pins it.

## Test gaps

Consolidated from all passes; ordered roughly by the findings they would have
caught.

- Nothing mirrors an arc, an arc with its tangency, or an axis-constrained
  shape about a tilted axis; nothing copies or transforms a
  SymmetricHorizontal pair (findings 1-3). Nothing rotates or scales a circle,
  a region or a composite; copy-isomorphism and isometry run on fixtures, not
  properties.
- No deep parameter-chain evaluation bound (finding 4); the alternative column
  never enters the fuzz churn; no version ≥ 2^31, malformed layer/style/colour,
  under-length seed list, or interleaved-unknown-record case; no SVG transform
  input; no additive-field round-trip.
- No type-then-click numeric commit; no Undo/Redo/Delete mid-gesture; no
  press-on-segment-body small drag; no shift+1/alt+1 through the shell's
  strokeOf — the entire QEvent translation is untested; no
  applicable-equals-runnable sweep under a locked layer.
- No async test edits between submit and drain (the epoch drop), none asserts
  dof/status under async, none exercises hidden-influence in an async
  component; the conflict-walk bound-exceeded path is unexercised.
- The bench gate is not in CTest (finding 22); the canaries miss three seams
  (finding 23); the locale test cannot fail in the sandbox (finding 25); CLI
  dispatch is unpinned.

## Verified sound

Areas re-verified during this pass and found to hold, recorded so the next
review can spend its attention elsewhere.

- Core: command inverses exact, refusal byte-preserving, deletionStep shrinks
  computed over the whole doomed set with shrinks before removals, watermark
  raise-only, taxonomy static_asserts covering alignment, alternatives and
  operand grouping, slots' DAG invariant and division-refusal, units
  parse-or-reject, topology's partition a deterministic function of the
  document, boundaryEnds/enclosesArea single-truth with no segment-shaped
  stragglers, ring direction recorded on the step and read by render and bake
  alike.
- Solve: the SPSC ring's ordering, the worker lifecycle and lost-wakeup
  handling, the solver gate covering sync solves while workers are alive
  (verified against the vendored global scratch), snapshot immutability, arena
  lifetimes with nothing escaping a heap, translation determinism with locked
  parameters in GROUP_BASE and fully-frozen constraints omitted, diagnose's
  redundancy backstop and bounded counterfactual walk with attributed-false
  meaning unattributable.
- Persistence: to_chars/from_chars throughout, ID-ordered byte-stable output,
  unknown-kind preservation to a fixed point, name escaping round-trips,
  forward references resolved and dangling ones refused, the loader
  loop-free on hostile input. SVG export's mask algebra: op-respecting
  polarity, nested intersect clips, punch carving its layer below its z and
  never a stroke, every inexpressible case counted, consistent y-convention
  both directions.
- Interact: pose as the single source for render and hit, refresh's
  per-component keep with failed components holding their last pose, the
  stage 7 rectangle-handle fixes complete on both commit paths including
  expression-slot refusal and the panel's check-before-drive, id claiming
  scanned past tool-claimed constraints under an atomic applyStep,
  pendingSnaps' queue lifecycle, marquee exactness via quadrant extremes,
  hidden-skipped-but-constraining, record-replay-record identity across every
  mutating action with refusals replaying as refusals, the generated
  catalogue's string_view storage never reallocating, usage ranking outside
  the journal, glyph anchors resolving for every operand shape with a
  whole-overlay budget.
- Render and shell: draw order fills-strokes-vertices per layer back-to-front,
  punch scoped to its saved layer, one tint rule, deviceScale as the single
  logical-device meeting point, grid handed down from the snap policy, fit
  latched until resetView, dimension text never writing display rounding back,
  the embedded typeface with a non-crashing parse fallback, paint synchronous
  with the GUI thread under the Image render target (a note for any future
  FBO switch), and the headless entry points touching no GL.
