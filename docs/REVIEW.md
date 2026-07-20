# Stage 5–6 review

A full review pass over the action surface and composition stages as of stage 6
exit, comparing what is implemented against PRINCIPLES.md and PLANS.md. Every
source file in src/ was read, the test suite was audited against the two
stages' per-stage test lists, and the highest-risk claims were verified against
the code rather than the comments — including the solver-semantics claims,
which were checked against the vendored SolveSpace equations directly.
Findings are numbered for reference and ordered by how expensive they get if
stage 7 builds on top of them.

The short version: both stages deliver what they promised. The registry is a
real single surface with a generated catalogue, imposition is movement-free
with the checks in the right order, booleans are genuinely records over live
operands, and the lock and hidden semantics match PRINCIPLES to the letter in
the solve path. The serious problems cluster in three places: the document has
grown a second definition of "closed" that disagrees with the first, the
composition actions have order- and lock-shaped holes their tests do not
reach, and several stage 5 surfaces stop one step short of the flow the plan
describes — the downgrade is computed but never offered, the preview verdict
is shown but the ghost pose is thrown away, and a walked conflict set cannot
actually be deleted.

Status: the correctness section, findings 1 through 8, is closed, along with
findings 10 and 16, which the fixes reached on their way. Each entry below
keeps its statement of the problem and records what the fix was; the test gaps
those findings left are closed with them. Findings 9 and 11 through 15, and the
remaining smaller notes, are open.

## What holds

Worth recording so the findings below read in proportion.

- Lock-as-solver-group is implemented exactly as amended: locked parameters
  join GROUP_BASE inside translate(), derived from the document so no caller
  can forget, fully frozen constraints are left out of the system, and
  seedTarget refuses to write a locked parameter. The corpus covers all four
  properties.
- The action registry is one table with real projections. The constraint
  catalogue is generated from CONSTRAINT_KINDS, the strip and palette read the
  same applicability the model validates with, keyboard resolution lives in
  resolveKey where tests reach it, and scripts refuse unknown actions at parse
  rather than dropping them at replay.
- Composition is compositional. A composite names live operands and is
  evaluated per frame in buildRegionPath; punch draws kClear inside a saved
  layer so it carves the layer and never the canvas; lift is a plain record
  removal and the operands reappear. No destructive boolean exists anywhere in
  the document model, and the bake is a value-producing projection.
- Degradation is a shrink computed over the whole doomed set, the
  whole-selection deletionStep exists for exactly the two-edges-of-one-loop
  case, regionState() is asked in one place by render and diagnostics alike,
  and a broken region draws a diagnostic rather than nothing.
- The imposition path is disciplined: capture from the pose, speculative check
  through a forked context, refusal (not silent downgrade) on the strip path,
  promotion re-captures and demotion does not, measure-once records nothing,
  and the hover-storm byte-identity test pins preview-does-not-mutate.
- Record → replay → record covers the new surfaces. Every composition action
  records under its registry name with resolved arguments, and the corpus
  gesture asserts the round trip through the file.

## Correctness — fixed

1. The document now has two definitions of "closed" and they disagree.
   Make-solid asks findBoundaryCycle (src/core/topology.h:117), which walks
   the full coincidence partition — transitive closure over every Coincident
   constraint. The fill then renders and degrades through boundaryRing
   (src/core/composition.cpp:181), whose JointClasses unions only coincidences
   *both of whose operands are ring endpoints* (composition.cpp:30–35). Two
   boundary endpoints joined transitively through an external point — a
   T-junction where a spur's endpoint sits between them, which snap chaining
   produces whenever the third click lands on the spur's point rather than a
   corner — are one joint to topology and two to JointClasses. Consequence: a
   loop the user can fill, and that make-solid accepts, immediately renders in
   the broken-diagnostic state. This is precisely the two-answers bug that
   moving the ring walk into core was meant to rule out, and the comment at
   composition.cpp:23 ("the answer is the same partition restricted to these
   points") is false — restriction of a transitive closure is not the closure
   of a restriction.

   Fixed. JointClasses unions every point any Coincident constraint names,
   interning points as it meets them, and the ring walk asks that partition
   about its endpoints. It stays a local union-find rather than becoming
   Topology — the question is smaller, Coincident alone with no ownership edges
   — but it answers the same as Topology::coincident now, and a test builds the
   T-junction and asks both definitions side by side.

2. A group drag moves locked geometry. The carried-translation loop in
   DragSession::update (src/interact/drag.cpp:214–228) writes
   origin-plus-delta into every carried point's seed span with no isLocked
   check. A locked parameter is a known — the solver takes its seed as its
   value — so writing the seed performs the move, and commit() then journals
   it. Group geometry on a locked layer together with unlocked geometry, drag
   the unlocked member: the locked layer's points translate rigidly and the
   result is committed. seedTarget refuses exactly this write for the grab
   (drag.cpp:61–63) and the refusal is documented as what makes a lock feel
   pinned; the carry path bypasses it.

   Fixed. DragSession::begin drops any carried entity whose component holds a
   lock, before the context is built and before carriedOrigin_ is filled — a
   whole component at a time, since skipping the locked spans alone would shear
   the rest of that component through its own constraints. A carried lock now
   reads as the same saturation a grabbed one does.

3. A selected relation cannot be deleted. Constraints are selectable through
   their glyphs and selectConflicting() makes the conflict set an ordinary
   selection — but deleteSelection (src/interact/session.cpp:1288) builds its
   step from selection_.items() alone, and edit.delete's applicability
   (src/interact/registry.cpp:27) reads the geometry signature, which is empty
   for a constraint-only selection. So the natural gesture — walk the
   conflicts, press Delete on the relation that is wrong — is a silent no-op,
   and the palette dims Delete while the conflict set is selected. PRINCIPLES
   is explicit that deletion of a relation uses the same selection machinery
   as geometry, and names it as what makes conflict sets walkable.

   Fixed. deletionStep gains a constraint span alongside its entity span, and
   deleteSelection passes both, so a selection reaching geometry and relations
   is one cascade whose shrinks are computed over the whole doomed set. Delete's
   applicability reads a new predicate that accepts either, and the counts
   report the relations because they are ordinary removals in the step. This is
   also where finding 10's tag shrinks landed; see there.

4. Make-solid and heal-and-fill attach the region to an arbitrary layer and
   style. regionOver (src/interact/session.cpp:1205–1211) takes
   layers().records().front() and styles().records().front() — the lowest-ID
   records, whatever the boundary's geometry says. Draw an outline on the base
   layer, create a layer through the palette, press F: the fill lands on the
   named layer while its outline stays on the base, so hiding or locking that
   layer splits the fill from the outline that defines it, and the z-order it
   competes in is the wrong layer's. The stage 5 comment above it defers the
   choice to stage 6; stage 6 landed layers and never came back. The corpus
   never sees it because every corpus fill happens before any layer exists.

   Fixed. regionOver takes the layer of the boundary's first edge — they share
   one in every reachable case — and no style at all, since a fill nobody styled
   reads as the outline it belongs to and inheriting the lowest-ID style meant
   picking up a stroke style that was never about it. A test draws on a named
   layer and fills.

5. Which region a subtract subtracts from is decided by creation order, not by
   the user. composeRegions (src/interact/session.cpp:1490) takes
   selectedRegions(), which is ID-ordered; render evaluates a composite as
   operands[0] minus the rest (src/render/view.cpp:137–151). So subtract
   always cuts the newer region out of the older one, there is no way to
   express the other reading, and the action takes no argument that could.
   This is a role ambiguity in exactly the sense PRINCIPLES sends to the
   surface — length-ratio asks which way round with preview; subtract, where
   the two readings differ far more visibly, never asks. The corpus happens to
   build the minuend first and cannot notice.

   Fixed. composeRegions sorts its operands by occlusion order — z, then ID, the
   same total order regionOrder uses — so the default cuts the upper region out
   of the lower one, which is what the picture looks like. region.subtract takes
   an optional `order` argument for the other reading, recorded the way an
   imposition records its assignment, and only subtract carries it because it is
   the only one of the three that can tell. The palette still does not ask;
   asking is a surface still to build, and the default now correlates with
   something visible.

6. walkConflicts reports a multi-way conflict as attributed.
   src/solve/diagnose.cpp:107 sets `attributed = true` unconditionally, while
   the comment eleven lines above it — and the header contract at
   diagnose.h:50–57 — say a conflict needing two simultaneous suppressions
   reports nothing and *leaves attributed false*. As written, a candidate that
   no single suppression rescues returns an empty set with attributed true,
   which is the exact "empty walkable set read as nothing conflicts" lie the
   header warns a surface cannot see through. Today every consumer happens to
   check both fields together, so the lie is latent; stage 7's compound
   relations make multi-constraint conflicts routine.

   Fixed. `attributed = !out.empty();`, with a semantics test that builds a
   conflict no single suppression rescues — two segments each held horizontal
   and also held parallel to each other, against a perpendicular candidate — and
   pins the empty-and-unattributed pair.

7. The angle residual accepts the supplementary angle and the solver does not.
   residual() for Angle (src/core/measure.cpp:191–195) returns
   min(|actual−declared|, |180−actual−declared|), justified by the comment
   "the solver constrains the direction cosine, so either the angle or its
   supplement satisfies the declaration". Checked against the vendored solver:
   the ANGLE equation is dircos − cos(valA) (external/solvespace/src/
   constrainteq.cpp:891–911), and cos(180−θ) = −cos(θ), so the supplement
   satisfies nothing unless `other` is set — which the taxonomy cannot set,
   because Angle carries no alternative. Consequences: the geometric residual
   is more permissive than the solver, so the semantics suite and the
   movement-free property would pass a translation bug that drives a segment
   to the supplement; and the solver's supplementary form is unrepresentable,
   the same shape as the tangency gap stage 5 fixed by adding `alternative`.
   Stage 7's rotate, which flips directions wholesale, is where the missing
   form starts to matter.

   Fixed both halves. The residual is |actual−declared| against the form the
   record names, and Angle and EqualAngle each carry an alternative in the
   taxonomy — which the solver was already reading as `other`, so the change is
   the column and nothing else. The alternative reverses the first direction,
   exactly as the solver's equation does, so the supplement is now a form of the
   same relation rather than an unrepresentable one. A semantics test checks the
   residual against the solver in both forms.

8. The bake silently loses geometry and mis-flattens nested composites.
   bakeForExport emits strokes only for segments (src/core/bake.cpp:76–89) —
   circles and arcs, stroked on every screen frame, are simply absent from the
   export projection, and no counter reports them, which breaks the loss
   report's own contract ("counting what it destroyed"). The composite walk
   (bake.cpp:51–71) tags every descendant outline with the *top* composite's
   op, so Intersect(A, Union(C,D)) bakes as A∩C∩D and Union(A, Intersect(C,D))
   as A∪C∪D; only subtract survives nesting by algebraic accident. And the
   walk is a stack that pops operands in reverse, so a subtract group's
   subtrahends precede its minuend in the paint-in-order list with nothing
   marking which is which. Stage 8's exporter will consume all three faults as
   truth.

   Fixed all three. Bake carries a BakedGroup list: one node per composite,
   naming the group it is itself an operand of, so Intersect(A, Union(C,D))
   arrives as a tree rather than as a flat intersection. Rings are emitted by
   recursion in operand order, which is what makes a subtract group's minuend
   the first of them. Circles and arcs are tessellated into polyline strokes at
   a fixed angular step — fixed rather than derived from a view, since an export
   has no zoom to be right at and a document must not bake differently from two
   sessions. Tests cover the nesting, the subtract order and both curve kinds.

## Model holes stage 7 will step into

Finding 10 is fixed, as a component of finding 3; the rest are open.

9. Layer and style removal leave dangling references the loader then refuses.
   RemoveRecord<LayerRecord> and RemoveRecord<StyleRecord>
   (src/core/document.cpp:338, :331) go straight to applyRemove with no
   dependents check, unlike entity, region and parameter removal — and no
   deletionStep overload exists for either. Entities and regions naming the
   removed layer or style keep the dangling ID, the document still serializes,
   and deserialize refuses it, because load validation checks layer and style
   references (document.cpp:86–87 via persist.cpp:764) — a state that can be
   saved and never reloaded, the same shape as the parameter hole the stage
   0–4 review closed (its finding 2). No action deletes a layer today, which
   is why nothing bleeds; the palette will grow one. Fix: HasDependents
   refusals mirroring entity removal, plus deletionStep overloads — for a
   layer, reassigning its entities and regions to the base layer before
   removal is the freeze-shaped answer (only the organization the user deleted
   is lost); for a style, nulling the references.

10. Constraint removal ignores the tags that list it, and the model gives
    three different answers about the resulting state.
    RemoveRecord<ConstraintRecord> (src/core/document.cpp:302) removes
    unconditionally; tagState (src/core/composition.cpp:262–264) treats a
    dangling tag→constraint reference as Broken, i.e. legal-but-degraded;
    validate(TagRecord) (document.cpp:193–196) refuses it, so the loader
    refuses the file it came from. Unreachable today only because nothing
    creates tags. Stage 7's rectangle tool creates one, and its defining
    constraints are exactly what decline and delete remove — the first
    declined inference on a rectangle produces a document that saves and will
    not load.

    Fixed, on the way through finding 3, and the answer kept is the refusal.
    RemoveRecord<ConstraintRecord> refuses while a tag names it, exactly as an
    entity removal does; deletionStep over constraints shrinks those tags first;
    and decline routes through deletionStep rather than emitting a bare removal,
    since a rectangle's squaring relations are precisely what decline takes. So
    a dangling tag reference is unreachable rather than variously legal, and
    validate and the loader go on refusing what no path can now produce.
    tagState's dangling branches stay as the defensive reading regionState has
    for a deleted edge. What a tag can still be is too thin to mean its kind,
    which is the degradation that was always the point.

11. Equal-angle's pairing ambiguity is never asked. Four segments admit three
    distinct pairings — angle(A,B) = angle(C,D) is not angle(A,C) =
    angle(B,D) — but EqualAngle is not orderSensitive in the taxonomy
    (src/core/taxonomy.h:216), so assignmentsFor (src/interact/impose.cpp:93)
    stops at the ID-lexicographically first valid permutation and the surface
    imposes a pairing the user never chose. This is the same question
    length-ratio got a surface for, on a kind where the wrong reading is
    harder to see. Fix: mark it orderSensitive and let the existing
    ambiguous-offer machinery enumerate the distinct pairings (the
    interchangeable-within-pair and pair-swap symmetries want deduplicating,
    which assignmentsFor's permutation walk can do by canonicalising each
    reading before keeping it).

## Stage 5 surfaces, thinner than the plan

12. The hover preview shows a verdict where the plan promised a ghost. PLANS
    stage 5 scope: "speculative hover preview of any offered constraint (ghost
    solve through contexts)"; PRINCIPLES: "the geometry ghosts to where commit
    would put it, which turns the catalogue into something learnable by
    looking". ImpositionPreview.pose carries exactly the overlay spans that
    ghost needs (src/interact/impose.h:119) and no surface consumes them:
    previewOf (src/shell/sketchview.cpp:529) reduces the preview to a string
    and the QML shows the string. The mechanism is built and tested; the
    payoff — seeing the drawing move before committing — is not wired.

13. The downgrade is computed and never actually offered.
    Presentation::downgradeOffered (src/interact/session.h:130) is a bare
    bool, though its comment claims it "names the kind and strength the offer
    would invoke"; nothing in the shell or QML reads it, the strip projects
    only Impose-strength rows (src/interact/surface.cpp:55), so after a
    refused imposition the user's one path to the reference measurement is
    knowing to search the palette for "(reference)" by hand. Stage 5's stated
    change was moving the choice to the user; the choice exists but the offer
    does not. Related staleness on the numeric path: commitPlacement sets
    impositionVerdict and conflicting but neither conflictAttributed nor
    downgradeOffered (src/interact/session.cpp:511–549), so both fields keep
    whatever the previous imposition left there. Fix: the presentation names
    the refused kind, stripEntries pushes the reference-strength entry while
    the offer stands, and commitPlacement resets all four fields together.

14. Constraint marks ignore layer visibility. visibleGlyphs
    (src/interact/glyphs.cpp:63) walks every constraint with no isVisible
    check, so hiding a layer removes its geometry from the canvas, hit
    testing and the marquee — while its relations' marks stay drawn, floating
    over empty space, and stay clickable, selecting relations on geometry the
    user cannot see or pick. Neither reading of the principles survives this
    halfway state: if hidden geometry cannot be aimed at, its marks should go
    with it; if no-invisible-constraints outranks that, the choice should be
    recorded and the marks should say what they are anchored to. As it stands
    the behaviour is an accident of omission, and the glyph budget spends
    screen slots on relations the user cannot act on.

15. "Is this region selected" has two answers. fillColourOf highlights the
    fill when any one boundary edge is selected (src/render/view.cpp:184–191);
    selectedRegions() requires every edge, recursively through composites
    (src/interact/session.cpp:1333–1348), and every region action goes through
    the latter. Select one edge of a filled square: the fill tints as selected
    and punch, raise, lower and subtract all refuse. One rule should own the
    question — the same discipline that moved the ring walk and the glyph
    fan-out into core — and the actions' rule is the documented one.

## Smaller notes

16. refresh() leaves a stale readout when the document empties: the dof and
    status update is guarded by components > 0 (src/interact/session.cpp:185),
    so deleting the last entity freezes the previous numbers on screen.

    Fixed. The guard is gone and an empty document reports zero degrees of
    freedom at status Okay: nothing has no freedom and nothing is wrong with it.

17. deleteSelection counts degradations (session.cpp:1298–1307) and no surface
    shows the count — status() reports shapes and relations only
    (src/shell/sketchview.cpp:434–439). The broken render carries the policy,
    but the third number is dead code until something says it.

18. applyDragResolve ignores applyStep's result (session.cpp:914–921) and then
    reads the newest constraint as `imposed` and notes usage; a refused step
    would attribute the imposition to an unrelated record. endDrag shares the
    ignored result but has nothing downstream of it.

19. make-solid stays applicable after filling — the loop is still closed, so
    the offer stays lit and a second F stacks an identical region over the
    first (nothing refuses a duplicate boundary). Harmless to the model,
    doubled alpha on screen, and an action that looks like it did nothing.

20. The crossing-segments refusal has no message. PLANS stage 5 asks for the
    loop-with-a-crossing rejection "with the deferred-case message per
    PRINCIPLES"; healableLoopContaining just returns nullopt
    (src/interact/loops.cpp:116) and no surface distinguishes "not healable"
    from "healable but crossing, which is the deferred case". The test pins
    the refusal, not the message, which does not exist.

21. toggleDriving's promotion never runs checkCandidate (session.cpp:1122), so
    promoting a reference to driving skips the redundancy flag the imposition
    path would raise for the identical declaration. Consistency cannot break —
    the captured value holds at the current pose — but redundancy is where
    later edits go to die, and the toggle is a quiet way to plant one.

22. validate(RegionRecord) permits a composite naming the same operand twice
    (document.cpp:167–181 checks cross-composite exclusivity and self-reference
    but not duplicates), which subtract renders as A−A. No surface can produce
    it; the command layer accepts it.

23. Esc during a drag does not cancel the drag: handle(Key::Escape)
    (session.cpp:782–812) falls through to selection ascend while drag_ runs
    on. Stage 3 territory, but stage 6's group-carry makes long drags more
    common and a cancel is the expected escape hatch.

24. moveLayer is the one layer mutation that skips refresh()
    (session.cpp:1470–1479). Nothing derived depends on order today, so it is
    an inconsistency rather than a bug; the sibling that later grows a
    dependency will copy the wrong precedent.

## Test gaps

The suite was strong on the paths the stages documented — lock-as-group,
hidden influence, degradation-and-restore, layer-order raster permutations,
the composition and flagship record/replay corpus — and thin exactly where the
findings above lived. The gaps belonging to findings 1 through 8, 10 and 16 are
closed with them:

- A boundary whose joints close transitively through a spur's endpoint, asked
  of both definitions of closed at once (finding 1). A fill drawn after a layer
  exists, checked for its layer and its absence of style (finding 4).
- A group holding locked and unlocked geometry, dragged, with the locked
  member's pose and seeds both unmoved (finding 2). Delete on a
  constraint-only selection, through the glyph that selects it, with the
  geometry surviving and undo restoring the relation (finding 3).
- Subtract with the operands raised and lowered past each other, so the reading
  follows the occlusion order rather than creation order, plus the reversed
  argument (finding 5). Bake tests for a nested composite's group tree, a
  subtract's ring order, a circle's closed polyline and an arc's open one
  (finding 8).
- A conflict needing two simultaneous suppressions, pinned as empty and
  unattributed (finding 6). The angle residual against the solver in both
  forms (finding 7).
- A tagged constraint removed on its own — refused bare, shrinking through
  deletionStep — and a cascade taking geometry and a named relation together in
  one shrink (finding 10).

What is still missing:

- The movement-free property runs on fixed fixtures; PLANS stage 5 asked for
  it over random solved documents. The angle residual is now checked against
  the solver directly, which is what would have caught finding 7, but the
  general property the plan asked for is not there.
- Round-trip property tests never remove a layer or style after references
  exist (finding 9) — the save-then-refuse state is reachable through the
  command layer the property tests drive.
