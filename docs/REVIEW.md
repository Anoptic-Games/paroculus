# Stage 0–4 review

A full review pass over the codebase as of stage 4 exit, comparing what is
implemented against PRINCIPLES.md and PLANS.md. Every source file in src/ was
read, the test suite was audited against the per-stage test lists, and the
highest-risk claims were verified against the code rather than the comments.
Findings are numbered for reference and ordered by how expensive they get if
stage 5 builds on top of them.

The short version: the architecture is real. The layer seams hold, the
determinism discipline is pervasive and genuine, the seeds thesis is
implemented coherently, and the semantics suite does what the plan promised.
The serious problems cluster in two places: number formatting is locale-unsafe
in both file formats, and the stage 4 inference plumbing is only fully wired
for the line tool — the circle, arc and rectangle tools break the WYSIWYG
contract in ways the current tests do not reach.

## What holds

Worth recording so the findings below read in proportion.

- The five-layer split is enforced, not aspirational: link boundaries, PRIVATE
  Skia/slvs linkage, boundary canaries in CTest. interact is genuinely free of
  Qt and Skia, which is why the gesture corpus runs headless.
- The command core is sound. Three command shapes with exact inverses,
  all-or-nothing composite steps with rollback, referential integrity as a
  model invariant, deletion expressed as a cascade with counts. The
  never-reuse-outranks-byte-identity watermark decision is reasoned in the
  right place and tested.
- Determinism is treated as a property, not a hope: ID-ordered tables and
  iteration everywhere, union-by-lowest-index in topology, deterministic
  tie-breaks in snap ranking, glyph ranking and hit testing, no hash-order
  dependence in solver translation, a seeded PRNG in the property tests.
- The seeds thread is coherent end to end: seeds serialize, solved state is
  never written back on open (and there is a test explaining why), drag commit
  is an ordinary command step, branch fixtures cover proximity, warm-start
  sweeps and cold re-solve.
- The semantics suite is per-entry as PLANS demanded: residual verification
  with off-constraint seeds, an infeasible variant per entry with blamed-set
  mapping checked, a redundancy variant per entry through checkCandidate. The
  three-signal redundancy policy in diagnose.cpp is well reasoned against
  SolveSpace's substitution behaviour.
- The taxonomy is one list with real projections, static_asserted against
  slvs.h constants in solve.cpp. Drag rollback on non-convergence, saturation
  attribution by suppression counterfactual, and the glyph budget all match
  their documented rationale.

## Fix before stage 5: correctness

1. Persist and script serialization are locale-dependent and will write
   unreadable files. `number()` in src/core/persist.cpp:30 and
   src/interact/script.cpp:17 uses `snprintf("%.17g")`, which honours
   LC_NUMERIC; parsing uses `std::from_chars`, which is locale-fixed to '.'.
   QGuiApplication calls `setlocale(LC_ALL, "")` on Unix, so the app adopts
   the user's locale. Verified on this machine: under de_DE.UTF-8,
   `%.17g` of 1.5 prints "1,5". Consequence: on any comma-decimal system,
   every save is silently unreadable by the loader, and serialization is not
   machine-independent — a direct violation of persist.h's stated contract.
   The tests never see it because the test runner never constructs a
   QGuiApplication. Fix: `std::to_chars` for doubles in both files (it is
   locale-independent and shortest-round-trip; the "%.17g is the shortest
   form" comment is also wrong — 17 significant digits prints 0.1 as
   0.10000000000000001, so to_chars improves diff legibility too). Same
   pattern in units.cpp formatLength is display-only and can stay, but note
   parseLength then rejects the comma the user's locale displays.

2. Removing a parameter leaves dangling references. RemoveRecord for
   ParameterRecord goes straight to applyRemove (src/core/document.cpp:238)
   with no dependents check, unlike entity removal. A constraint slot
   referencing the removed parameter then evaluates to nullopt, and
   translation applies `value_or(0.0)` (src/solve/solve.cpp:249) — the
   dimension silently drives to zero, the worst kind of silent change. Worse:
   the document still serializes, but deserialize refuses it, because load
   validation checks `parameters_.contains` for constraint slots — a state
   that can be saved and never reloaded. Parameter-to-parameter references
   have the same hole and load fine (only the cycle check runs on parameters
   at load), so validation is inconsistent with itself. Style slots are not
   validated at all. Fix: a dependents check over slots (constraints, styles,
   parameters) mirroring entity removal, a deletionStep-style cascade or
   refusal, and load-time validation for parameter and style slot references.

3. Circle radii are translated from the document, not the solve context. In
   the Circle case of translate() (src/solve/solve.cpp:165) the radius
   parameter is seeded from `e->seeds[0]` — the committed document seed —
   while points are seeded from the context's spans. The context is supposed
   to be the parameter store ("seeds going in, solved values coming out");
   this breaks warm-start continuity for every radius across a drag (each
   frame re-seeds the radius from the committed value), and any speculative
   context that perturbs a radius is silently ignored. Fix: look the span up
   by entity, as the readback loop already does.

4. Dragging a circle is nonsense masked into inertness. DragSession::begin
   accepts any entity with own parameters, and update()
   (src/interact/drag.cpp:73) writes cursor.x/cursor.y into the grabbed
   entity's span unconditionally — for a circle that assigns radius :=
   cursor.x. Finding 3 currently hides this by discarding the context's
   radius, so the observable behaviour is that grabbing a circle's rim does
   nothing. Fixing 3 alone would expose 4 (radius jumps to the cursor's x
   coordinate). Either refuse non-point grabs in begin() or implement a real
   radius drag (radius := |cursor − centre|). At present hit-testing happily
   returns circles, so users will hit this immediately.

5. Inference subject plumbing is only correct for the line tool. Session's
   press path declares the placement's auto-committed candidates against
   `out.placedPoint` / `out.placedSegment` (src/interact/session.cpp:360),
   and holds first-click snaps for `out.placedStart`. Audit per tool:
   - CircleTool sets placedPoint to the centre (src/interact/tools.cpp:229),
     but the second click's inference runs at the rim. Snapping the rim onto
     an existing point declares Coincident(centre, thatPoint) — the circle
     teleports so its centre sits on the point the rim touched. The correct
     declaration for a rim snap is PointOnCircle(target, circle), or nothing.
   - ArcTool never sets placedStart, so the snaps captured when the user
     started the arc on an existing endpoint are silently dropped; and the
     second (end) click overwrites pendingStartSnaps_ in the
     `commands.empty()` branch (session.cpp:324), so the mechanism can only
     remember one pending click while the arc gesture has two. Arc endpoints
     therefore never receive the coincidences the user aimed for.
   - RectangleTool never sets placedPoint, so the closing corner's endpoint
     snap is position-corrected but declares nothing — the rectangle merely
     sits on the vertex instead of being bound to it.
   - Ghost glyphs render any auto-committing candidate
     (src/interact/glyphs.cpp:115), including segment-subject candidates that
     constraintFor will drop because the tool created no segment — so for
     circle/arc/rect gestures the preview promises relations commit will not
     deliver. "Preview shows truth" currently holds for lines only.
   The WYSIWYG test (tests/unit/interact_snap.cpp:233) covers only the line
   tool, which is why all of this is green. This block is the most expensive
   finding to defer: stage 5's heal-and-fill and make-solid lean directly on
   endpoint inference being trustworthy for every tool.

6. Record/replay fidelity breaks for key-driven numeric entry.
   Session::handle(Key) records the key step, then dispatches to
   numericResolve/numericAdvance/numericCancel, each of which also records
   its own step (session.cpp:170,183). A live Tab advances once but records
   two steps ("key name=tab" plus "numeric do=advance"), so replay advances
   twice and the replayed session diverges from the recorded one. Re-recording
   a replay appends a third step per Enter/Tab/Esc, so record → replay →
   record is not the identity — the property CLAUDE.md states and the format
   exists to guarantee. The identity test (tests/unit/interact_script.cpp:112)
   only records a drag, and the numeric round-trip test never re-records,
   which is exactly the gap. Fix: the numeric methods should record only when
   invoked directly (e.g. an internal non-recording variant used by
   handle(Key)), or handle(Key) should not double-dispatch to recording
   methods. Related: a typed space serializes as "type char= " which the
   parser refuses — the recorder can produce an unparseable file.

7. The numeric twin's exactness does not survive the commit click in the real
   app. numericResolve pins the tool's cursor (the preview shows exactly 45),
   but the commit press re-runs inference at the pointer's actual position
   and overwrites it — LineTool::press assigns `cursor_ = cursor`
   unconditionally — so a plain-Enter resolve is discarded by the very click
   that commits it, even if the physical mouse never moved (its position is
   still the pre-Enter one). Shift+Enter appears to work only because the
   imposed dimension drags the geometry to the value after commit. The tests
   pass because they click exactly at preview.to (interact_numeric.cpp:87), a
   move only a script can make. Decide what Enter means and implement it:
   either Enter commits the placement at the resolved pose, or a resolved
   parameter must survive until commit and override the press position.

8. In the shell, digits 1–9 cannot begin numeric entry.
   SketchView::keyPressEvent routes 1–9 to confirm/decline before the
   open-a-field branch (src/shell/sketchview.cpp:259), so with any offer
   visible, typing "45" confirms offer 4 and drops the 5; with none, the
   keystroke does nothing. A field can only be opened by Tab, '0', '.' or
   '-'. CLAUDE.md's "digits / tab — type a value into the strip" does not
   match the code. This wants a deliberate policy (e.g. digits type while a
   tool has a placement in flight, confirm requires a modifier — or the
   reverse), recorded in the corpus either way.

9. imposePending_ survives Esc. Type a value, Shift+Enter, Esc to abandon the
   chain, draw a fresh shape: the stale dimension (with the old value and
   target index) is imposed on the new geometry
   (session.cpp:366,490). Esc and tool_->escape() must clear it, as they
   already clear confirmedOffers_ and pendingStartSnaps_.

10. quote() does not escape newlines (src/core/persist.cpp:38). A layer,
    style, group or parameter name containing '\n' splits its record line;
    the round trip silently corrupts the name and sheds fields into an
    unknown-record line rather than failing. Names are arbitrary strings via
    the command layer, so this is reachable. Escape control characters, and
    add newline names to the names-survive test.

11. Construction segments attract parallel/perpendicular. The direction-kind
    loop in snap() iterates every document segment with no role check
    (src/interact/snap.cpp:165), while the point-kind loop honours
    policy.snapToConstruction. An arc's flanks stay quiet, but any
    construction segment becomes exactly the magnet the policy exists to
    prevent. One-line fix plus a test mirroring "an arc's centre does not
    attract".

## Divergences from PRINCIPLES and PLANS

12. Horizontal and vertical are not axis-referenced. PRINCIPLES fixes them as
    parallelism to a reference axis with the document frame as default;
    the taxonomy implements one-operand kinds and defers the reference to
    stage 7 (src/core/taxonomy.h:129). Acceptable pre-freeze, but the
    retrofit changes the operand signature, and with it persist, undo
    records, signatureMatches, and the corpus. Stage 7's rotate-with-retarget
    is blocked on it. Decide now whether the operand arrives as a nullable
    reference (null = document frame) so the format change lands once.

13. Double-click depth descent does not exist. Selection::descend is
    implemented and unit-tested but has no caller; PointerEvent carries no
    click count and the shell never synthesizes one. Consequently depth_ is
    always zero in the app and Esc-ascend is equally unreachable. This is
    stage 3 scope ("double-click depth descent along coincidence runs, Esc
    ascent") reported as complete. Also, descend() replaces segments with
    their endpoints rather than runs with their segments as the header
    comment describes — worth reconciling when it gets wired.

14. Multi-selection drag drags one entity. PRINCIPLES: "Multi-selection drags
    put all selected parameters in the set." DragSession::begin takes a
    single grabbed id and the dragged set is only that entity
    (src/interact/drag.cpp:80). SolveOptions.dragged is already a vector, and
    the click-inside-selection-keeps-it logic in Session::handle is already
    there waiting; the gap is contained in begin()/update().

15. The arc tool has no numeric twin. Its radius and sweep parameters are
    display-only — no setParameter, no dimensionFor — so typing during an arc
    gesture does nothing. Stage 4's goal line is "every gesture gains its
    numeric twin". Also PLANS' numeric-twin test list starts "drag, type,
    enter"; there is no numeric path for drags of existing geometry at all
    (Session::type requires an active creation tool). If drag-numeric is
    deliberately stage 5 (inline dimension editing), amend the plan text;
    the arc gap is just a hole.

16. checkCandidate has no callers. The creation-time speculative check —
    stage 2 scope, and PRINCIPLES' first over-constraint moment — is built
    and tested but wired to nothing: imposed dimensions (Shift+Enter) and
    auto-committed inferences land unchecked. A contradictory typed dimension
    commits successfully and only manifests through finding 17's failure
    mode. The action surface that offers downgrades is stage 5, but stage 4
    already creates constraints; at minimum the imposition path should
    consult it.

17. A single failing component un-solves the whole canvas. refresh() solves
    the entire document as one context and, on any non-OK outcome, discards
    the settled overlay entirely (src/interact/session.cpp:61) — every
    healthy component's display falls back to committed seeds. PRINCIPLES:
    the document stays editable and geometry holds the last feasible
    solution. The partition exists; refresh() should solve per component and
    keep the components that solve. This also removes the whole-document
    solve from the per-edit path, which the scale hypothesis will eventually
    demand anyway.

18. Incremental topology maintenance is dead code. Session only ever calls
    markDirty() and rebuilds; noteAdded() has no callers outside the unit
    test, so the union-in-place path the design documents (and the stage 1
    property test pins) never runs in the product. Rebuild-always is correct
    and currently cheap; either wire noteAdded into the journal path or
    delete it and its test until scale asks for it — an invariant tested on
    unreachable code is a false sense of coverage.

19. Undo seed spans are vestigial. UndoRecord.seedsBefore is never written,
    recordSeedsAfter is never called from src/, and undo()/redo() never read
    either field. Branch fidelity is in fact carried by seed commits being
    ordinary SetRecord commands with exact inverses — which works and is
    tested — so "every undo record carries the seeds of affected components"
    (PLANS stage 1) is satisfied by a different mechanism than the one built
    for it. Decide: remove the fields, or wire them. Leaving them
    half-present invites stage 8's async work to trust them.

20. Tangent cannot say which end. SLVS_C_ARC_LINE_TANGENT reads
    Slvs_Constraint.other to select the arc end; translate() zero-fills it,
    so tangent always means tangent-at-start and the taxonomy has no way to
    express the alternative. Stage 5 makes the whole catalogue imposable;
    imposing tangency on the far end is unrepresentable today. Needs a field
    on ConstraintRecord (and persist) or a second taxonomy row.

## Smaller gaps stage 5 will trip over

21. Marquee ignores circles and arcs (src/interact/hit.cpp:193) — only
    points and segments are tested for containment, so a marquee over a
    circle selects its centre but not the circle. Written in stage 3, never
    extended for stage 4's entities.

22. Glyph marks skip arc operands. anchorOn() (src/interact/glyphs.cpp:13)
    handles points, segments and circles but returns nullopt for an arc, so
    a constraint binding an arc gets no mark on it — the arc macro's own
    point-on-circle is invisible from the arc side, violating
    mark-per-operand. Route through curveCentre/curveRadius or the arc
    midpoint.

23. Circles render without hover or resisting tint (src/render/view.cpp:272
    picks only selected/construction), unlike segments, arcs and points — an
    attribution gap once circles resist a drag.

24. The rendered grid and the snap grid are two constants. renderDocument
    hardcodes 20.0 (src/render/view.cpp:176); SnapPolicy.gridStep is the
    policy. Change the policy and the drawn grid lies about where placement
    lands. Render should receive the step from the policy via the shell.

25. Shell view state is thinner than stage 3 claims: pan exists as a member
    but nothing writes it (middle-drag is accepted then ignored), zoom is
    viewport-centre anchored rather than cursor-anchored, and — more
    disruptive — syncViewport() re-fits the base framing to the geometry
    bounding box on every keypress, so confirming an offer with '1' mid-chain
    visibly reframes the view under the cursor. Scripts survive (viewport
    steps are recorded) but the feel is wrong and any pixel-calibrated
    tolerance changes meaning when the view jumps.

26. Deleting an entity that belongs to a group deletes the whole group
    record (deletionStep emits RemoveRecord<GroupRecord>). Regions and tags
    are documented as stage 6 degradation deferrals; groups are not
    mentioned anywhere. A SetRecord shrinking the membership preserves the
    group and is expressible today.

27. A two-segment loop (two edges joining the same two vertices) satisfies
    findBoundaryCycle's degree and connectivity tests and reports a closed
    2-gon. Harmless until make-solid fills it; refuse cycles shorter than 3
    unless a lens shape is intended.

## Test gaps against the plan's own lists

- WYSIWYG is asserted only for the line tool; there are no
  previewed-equals-committed tests for circle, arc or rectangle placements —
  precisely where finding 5 lives. The rim-snap test checks the offer is
  generated, never what confirming it declares.
- The "inference precision/recall corpus opening set" (stage 4 test list)
  does not exist under any name; the corpus is stage 3 gestures only.
- Record→replay→record identity is tested only over a pointer/key drag
  session; no test re-records a session containing numeric or confirm steps
  (finding 6's territory).
- Arc macro invariants cover the through-point and centre exclusion, but I
  found no test for "degradation on member deletion" (stage 4 list).
- The persist name-robustness test covers spaces and quotes but not newlines
  or other control characters (finding 10).
- Nothing exercises persist under a non-C locale (finding 1). After the
  to_chars fix, a test can pin the property by asserting no ',' in
  serialized output for a fractional value, plus a byte-identity fixture.

## Minor notes

- document.cpp validate(EntityRecord) canonicalizes unused point slots but
  not unused seed slots, so a segment with junk in seeds[] is accepted,
  compares unequal to its own round-trip, and quietly loses the junk through
  persist. Mirror the points check.
- The MissingValue comment claims both directions ("a valued constraint with
  no slot, or vice versa") but only the valueless-with-value half is
  checkable — constant 0.0 is a legitimate distance. Trim the comment.
- persist.h says a failed deserialize leaves `out` empty; it actually leaves
  it untouched (parseScript got this right). Align code or comment.
- Watermark handling on load uses setNext, which can lower a counter already
  raised by reserveAbove if a hand-edited file orders records before the
  watermark line; make it a max.
- Topology::noteAdded(ConstraintId) marks dirty when operand 0 is unknown but
  silently skips unknown later operands; unreachable today, inconsistent
  regardless. Also noteAdded renumbers O(n) per record — a rectangle's 16
  records would be 16 linear passes if it were ever wired.
- UndoJournal::undo/redo ignore per-command apply() results; an assert would
  catch a journal desync (mutation behind the journal's back) at the moment
  it happens instead of at the next serialize.
- checkCandidate's conflicting set can include the candidate itself (a null
  id when the caller left it unset); stage 5's conflict walking should filter
  or the highlight will contain a ghost.
- Session::handle(Esc) inside a tool clears the chain but leaves
  presentation_.snapCandidates until the next move — stale ghosts for a
  frame or two.
- rememberSnaps runs even when the placement step was refused, so a failed
  placement still bumps recency ranking.
- arena.h array() uses placement array-new, which the standard permits to
  add a size cookie; fine on Itanium ABI for trivially-destructible types,
  but a loop of singular placement-new (or ::operator new-style raw
  construction) removes the assumption.
- connectedRun's "up" step scans every entity per reached entity — O(n²) per
  click; fine now, worth an adjacency index when documents grow.

## Suggested order

Findings 1 and 2 are data-loss class and independent of everything else; fix
first. Findings 3+4 travel together (fixing 3 exposes 4). Finding 5 is the
largest single piece of work and gates stage 5's inference-dependent scope
(heal-and-fill, make-solid); 6–9 are small once decided. 12, 19 and 20 are
decisions to make on paper before stage 5 amends the plan; 16 and 17 land
naturally with stage 5's imposition and conflict work but should be named in
its scope rather than discovered during it.
