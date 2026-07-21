# The document format

This is the frozen specification of the paroculus document format, the file a
save writes and an open reads. It is the declaration layer plus seeds plus roles
and tags — authoring intent and branch choice, nothing rebuildable. There are no
tessellations, no spatial indexes, no component partition on disk; all of those
are derived and would only rot there.

The format is version 0, and stage 8 freezes it. What freezing means is written
in the migration policy at the end: before the freeze, a breaking change
regenerated the corpus and grew no shim; after it, a breaking change bumps the
version and ships a read shim, and additive change keeps the version and rides
the unknown-record mechanism instead.

The reference implementation is `src/core/persist.cpp`. Where this document and
that file disagree, the file is right and this document is the bug.

## Shape

A document is a sequence of newline-terminated lines in UTF-8. The first line is
the header; every line after it is one record. A record is a kind token followed
by space-delimited `key=value` fields, except that the id-bearing records carry
their id as the first bare token after the kind.

```
paroculus 0
watermark entity=7 constraint=3 region=2 tag=1 style=1 layer=1 group=1 parameter=1
layer 1 name="guides" order=-3 visible=0 locked=1
entity 1 kind=point role=normal layer=0 points=- seeds=0,0
constraint 1 kind=horizontal driving=1 operands=5
region 1 style=0 layer=0 boundary=5,6,7
```

Four properties the shape holds, each tested:

- versioned, from the first byte. The header names the version so a reader knows
  what it is looking at and a migration has something to migrate from.
- stably ordered. Records come out in id order within each section and the
  sections come out in a fixed order, so the byte output is a function of the
  document alone — a diff is readable, a merge is sane, and byte-stability is a
  property rather than an accident of hash iteration.
- lossless. Doubles round-trip exactly; display rounding lives at the
  presentation boundary and never reaches storage.
- forward-safe, at the same format version. A record kind a reader does not know
  survives a round-trip verbatim, so an older build saving a newer *additive*
  file sheds nothing it did not understand. A file at a greater version is a
  breaking change and is refused whole rather than half-understood — see the
  migration policy at the end.

## Header

```
paroculus <version>
```

The version is a single non-negative integer, currently 0. A file whose version
is greater than the reader's is refused whole rather than half-read: a greater
version is a breaking change the reader cannot claim to understand well enough to
write back. A file whose version is less than or equal to the reader's loads.

## Lexical rules

Fields are separated by single spaces. A value that must hold a space, a quote,
or a control character is a quoted string; every other value is bare.

Numbers are written with `std::to_chars` and read with `std::from_chars`, which
are locale-independent and fixed to `.` as the decimal point. The printf family
is never used, because Qt calls `setlocale(LC_ALL, "")` on Unix and a `%g` save
would write `1,5` under a comma-decimal locale and then refuse to read it back.
`to_chars` is also shortest-round-trip, so `0.1` writes as `0.1` rather than as
seventeen digits.

Quoted strings open and close with `"`. Inside, `\` escapes the next character,
and a control character (below 0x20, or 0x7f) is written `\xHH` with two
lowercase hex digits. A name holding a newline would otherwise split its own
record: the loader reads a line at a time, so the tail would arrive as an
unrecognised record and be kept as an unknown one, silently truncating the name
and growing the file. Names are arbitrary strings through the command layer, so
this is reachable, not theoretical.

An id list is comma-separated non-negative integers, or `-` when empty. A seed
list is comma-separated numbers, or `-` when the kind owns no parameters.

A slot is either a bare number — the constant, which is the trivial expression —
or a bracketed expression `[node;node;...]`. Each node is one of:

- `c<number>` a constant leaf,
- `p<id>` a reference to a named parameter,
- `~<index>` negation of an earlier node,
- `+<a>,<b>` `-<a>,<b>` `*<a>,<b>` `/<a>,<b>` a binary op over two earlier nodes.

Node operands index strictly earlier nodes, which is the invariant that makes
evaluation one forward pass and self-reference impossible. Nodes are written with
explicit indices rather than as postfix because a loaded expression may share
subtrees and postfix would silently unshare them.

## Records

Sections appear in this order, and the id-ordered records of each follow the
one before: `watermark`, then `layer`, `style`, `parameter`, `entity`,
`constraint`, `region`, `tag`, `group`, `usage`, then unknown records verbatim.
The order is dependency order — a layer before the entity that names it — so a
loader that validated per line would still see every reference resolve, though
in fact it validates once over the finished document.

### watermark

```
watermark entity=<n> constraint=<n> region=<n> tag=<n> style=<n> layer=<n> group=<n> parameter=<n>
```

The next id each allocator will hand out. Without it a reopened document would
reissue an id a deleted record once held, and any surviving reference to it would
silently rebind. On load the watermark only raises the line, never lowers it,
because records reserve above themselves as they load and a hand-edited file may
place the watermark last.

### layer

```
layer <id> name=<string> order=<int> visible=<0|1> locked=<0|1>
```

`order` is signed: a layer may sit below the implicit base layer, which is the
null id and is never written.

### style

```
style <id> name=<string> stroke-width=<slot> stroke=<uint> fill=<uint> filled=<0|1> [opacity=<slot>]
```

`stroke` and `fill` are packed RGBA integers, not quantities arithmetic applies
to, so they are not slots. `opacity` is written only when it is not the fully
opaque `1.0`: a document that does not use it writes the line it always wrote,
which is the rule every field added after the format existed follows.

### parameter

```
parameter <id> name=<string> value=<slot>
```

### entity

```
entity <id> kind=<point|segment|circle|arc> role=<normal|construction> layer=<id> [style=<id>] points=<idlist> seeds=<seedlist>
```

`points` holds exactly the kind's point count; `seeds` holds exactly its own
parameter count. Seeds are the record of which branch the user was shown, so they
are written even when they are still zero. Unused seed and point slots are
canonical: junk left in a circle's second seed would survive the commit, would
serialize, and would make a record compare unequal to its own round-trip while
nothing on screen ever showed it.

### constraint

```
constraint <id> kind=<name> driving=<0|1> operands=<idlist> [value=<slot>] [alt=<uint>]
```

`operands` holds the kind's required operands plus as many of its optional ones
as the record binds — an unreferenced horizontal writes its one operand exactly
as it did before the nullable reference axis existed. `value` appears when the
kind has value arity one. `alt` is the constraint's alternative form and is
written only when it is not the default `0`: tangency reads it to pick the arc
end it holds at, and every other kind leaves it default and writes no field.

### region

```
region <id> style=<id> layer=<id> boundary=<idlist> [op=<name> operands=<idlist>] [z=<int>] [punch=1]
```

A plain filled outline writes only the first four fields, exactly the line it
wrote before the region algebra existed. `op` is a composite operation named
rather than numbered, so inserting an operation later cannot reinterpret a file
written before it; when present it comes with `operands`, the child regions it
combines. `z` is a signed order within the layer, written only when non-zero.
`punch` marks an alpha-overwrite region and is written only when set.

### tag

```
tag <id> kind=<rectangle|distribution|mirror> entities=<idlist> constraints=<idlist>
```

A tag owns nothing. It names the primitives and constraints that give it its
identity, and when an edit breaks one of them the tag degrades — it is not
removed, and what it still names is untouched.

### group

```
group <id> name=<string> members=<idlist>
```

### usage

```
usage kind=<name> count=<uint>
```

Ancillary and droppable: how many times this document has reached for each
relation, which is the whole of what the context strip ranks by. A build that
deletes these lines opens the same drawing, ranked by taxonomy order alone.
Written in taxonomy order and only for kinds with a non-zero count, so a document
that imposed nothing writes no usage section rather than twenty-two zeroes. A
malformed usage line is still refused like any other, because a file that says
something has to say it correctly or a round-trip is not a round-trip.

### Unknown records

A line whose kind token the reader does not recognise is kept verbatim, in the
order it arrived, and written back after every known section. This is the whole
of forward compatibility: a newer build adds a record kind, an older build
preserves it, and no save from the older build truncates the newer file. Because
the unknowns are emitted last and the known sections are stably ordered, a file
reaches a byte fixed point after one save rather than shuffling on every one.

## Migration policy

The format is frozen at version 0. The freeze changes what a breaking change
costs, not whether one is allowed.

- Additive change keeps version 0. A new record kind, or a new optional field on
  an existing kind that older readers can ignore, does not bump the version: old
  readers preserve the new records through the unknown mechanism and skip fields
  they do not know. This is why the version has stayed 0 across the additions
  that arrived through stages 4 through 7 — the nullable reference axis, the
  tangent alternative, the region algebra, opacity — none of which an older
  reader chokes on.
- Breaking change bumps the version and ships a read shim. A change that
  reinterprets an existing field, removes one, or changes the meaning of a record
  is not something an old reader can be trusted to round-trip, so it raises the
  version. The new reader keeps a shim that reads the old version and writes the
  new; it never silently reinterprets an old file as a new one, which is why a
  greater version than the reader's is refused rather than guessed at.
- Before the freeze there were no shims. A breaking change regenerated the
  checked-in corpus and moved on, because there were no files in the wild to
  protect. That era ends here: the corpus under `tests/corpus` and the
  round-trip properties in `tests/unit/core_persist.cpp` are the contract a shim
  must keep from now on.

The forward-compatibility corpus, `tests/corpus/future.paro`, is a version-0
document carrying record kinds no build in this tree defines. It is what a
future additive version looks like to a reader that predates it, and the test
that opens and re-saves it asserts the promise this policy rests on: nothing is
shed.
