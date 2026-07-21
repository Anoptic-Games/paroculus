#include <doctest/doctest.h>

#include <clocale>
#include <fstream>
#include <sstream>

#include "core/persist.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::Rng;
using paroculus::test::addArc;
using paroculus::test::addCircle;
using paroculus::test::addConstraint;
using paroculus::test::addPoint;
using paroculus::test::addSegment;

namespace {

// A document exercising every record kind, so round-trip properties are not
// quietly passing over tables nothing ever populated.
Document richDocument() {
    Document doc;

    LayerRecord layer;
    layer.name = "guides and \"quotes\"";
    layer.order = -3;
    layer.visible = false;
    layer.locked = true;
    const LayerId lid(doc.apply(AddRecord<LayerRecord>{layer}).allocated);

    StyleRecord style;
    style.name = "hairline";
    style.strokeWidth = Slot(0.25);
    style.strokeColor = 0xff3366ccu;
    style.filled = true;
    style.opacity = Slot(0.5);
    const StyleId sid(doc.apply(AddRecord<StyleRecord>{style}).allocated);

    ParameterRecord gutter;
    gutter.name = "gutter";
    gutter.value = Slot(8.0);
    const ParameterId gid(doc.apply(AddRecord<ParameterRecord>{gutter}).allocated);

    ParameterRecord margin;
    margin.name = "margin";
    margin.value = Slot::binary(ExprOp::Multiply, Slot::parameter(gid), Slot(2.0));
    doc.apply(AddRecord<ParameterRecord>{margin});

    // A square with coincident corners.
    std::vector<EntityId> pts, edges;
    const double xs[4] = {0.0, 10.0, 10.0, 0.0};
    const double ys[4] = {0.0, 0.0, 10.0, 10.0};
    for(int i = 0; i < 4; i++) {
        const int j = (i + 1) % 4;
        const EntityId a = addPoint(doc, xs[i], ys[i]);
        const EntityId b = addPoint(doc, xs[j], ys[j]);
        pts.push_back(a);
        pts.push_back(b);
        edges.push_back(addSegment(doc, a, b));
    }
    for(int i = 0; i < 4; i++) {
        addConstraint(doc, ConstraintKind::Coincident, {pts[i * 2 + 1], pts[((i + 1) % 4) * 2]});
    }
    addConstraint(doc, ConstraintKind::Horizontal, {edges[0]});
    // A valued constraint whose value is an expression over a parameter.
    addConstraint(doc, ConstraintKind::PointPointDistance, {pts[0], pts[1]},
                  Slot::binary(ExprOp::Add, Slot::parameter(gid), Slot(1.5)));
    // A construction-role edge and a circle with a radius seed.
    EntityRecord guide = *doc.entities().find(edges[3]);
    guide.role = Role::Construction;
    guide.layer = lid;
    doc.apply(SetRecord<EntityRecord>{guide});

    const EntityId centre = addPoint(doc, 5.0, 5.0);
    EntityRecord circle;
    circle.kind = EntityKind::Circle;
    circle.points = {centre, EntityId(), EntityId()};
    circle.seeds = {3.5, 0.0};
    const EntityId cid(doc.apply(AddRecord<EntityRecord>{circle}).allocated);
    addConstraint(doc, ConstraintKind::Radius, {cid}, Slot(3.5));

    // An arc, so the curved-entity seeds and three-point form are in the corpus.
    const EntityId arcStart = addPoint(doc, 8.5, 5.0);
    const EntityId arcEnd = addPoint(doc, 5.0, 8.5);
    paroculus::test::addArc(doc, centre, arcStart, arcEnd);

    // A reference measurement — non-driving, so the driving flag round-trips in
    // both states rather than only the one every other constraint sets.
    ConstraintRecord reference;
    reference.kind = ConstraintKind::PointPointDistance;
    reference.operands[0] = pts[2];
    reference.operands[1] = pts[3];
    reference.driving = false;
    reference.value = Slot(9.5);
    doc.apply(AddRecord<ConstraintRecord>{reference});

    // The square as an outline, the circle as one, and a punched composite over
    // the two — so op, operands, z and punch are all exercised, not just the
    // plain-outline line every other region test uses.
    RegionRecord square;
    square.boundary = edges;
    square.style = sid;
    square.layer = lid;
    const RegionId squareId(doc.apply(AddRecord<RegionRecord>{square}).allocated);

    RegionRecord disc;
    disc.boundary = {cid};
    disc.style = sid;
    disc.layer = lid;
    const RegionId discId(doc.apply(AddRecord<RegionRecord>{disc}).allocated);

    RegionRecord cut;
    cut.op = CompositeOp::Subtract;
    cut.operands = {squareId, discId};
    cut.layer = lid;
    cut.z = 2;
    cut.punch = true;
    doc.apply(AddRecord<RegionRecord>{cut});

    // A union and an intersect over fresh disc operands, so both composite tokens
    // round-trip — the subtract above is not the only op the freeze suite sees.
    auto discRegion = [&](double cx, double cy) -> RegionId {
        const EntityId c = addPoint(doc, cx, cy);
        const EntityId circle = addCircle(doc, c, 2.0);
        RegionRecord reg;
        reg.boundary = {circle};
        reg.layer = lid;
        return RegionId(doc.apply(AddRecord<RegionRecord>{reg}).allocated);
    };
    RegionRecord uni;
    uni.op = CompositeOp::Union;
    uni.operands = {discRegion(20.0, 20.0), discRegion(24.0, 20.0)};
    uni.layer = lid;
    doc.apply(AddRecord<RegionRecord>{uni});
    RegionRecord inter;
    inter.op = CompositeOp::Intersect;
    inter.operands = {discRegion(30.0, 20.0), discRegion(34.0, 20.0)};
    inter.layer = lid;
    doc.apply(AddRecord<RegionRecord>{inter});

    const ConstraintId firstConstraint = doc.constraints().records().front().id;
    TagRecord tag;
    tag.kind = TagKind::Rectangle;
    tag.entities = edges;
    tag.constraints = {firstConstraint};
    doc.apply(AddRecord<TagRecord>{tag});

    // A mirror tag and a distribution tag, so those kind tokens round-trip too.
    TagRecord mirror;
    mirror.kind = TagKind::Mirror;
    mirror.entities = {edges[0], edges[1]};
    mirror.constraints = {firstConstraint};
    doc.apply(AddRecord<TagRecord>{mirror});
    TagRecord distribution;
    distribution.kind = TagKind::Distribution;
    distribution.entities = {pts[0], pts[2], pts[4]};
    doc.apply(AddRecord<TagRecord>{distribution});

    GroupRecord group;
    group.name = "frame";
    group.members = edges;
    doc.apply(AddRecord<GroupRecord>{group});

    // A non-empty usage section, so the ranking history's write and parse paths
    // are round-tripped rather than skipped by a document that imposed nothing.
    doc.noteUsage(ConstraintKind::Parallel);
    doc.noteUsage(ConstraintKind::Parallel);
    doc.noteUsage(ConstraintKind::EqualLength);

    return doc;
}

}  // namespace

TEST_CASE("the format is versioned from the first write") {
    const std::string text = serialize(Document());
    CHECK(text.rfind("paroculus 0\n", 0) == 0);
}

TEST_CASE("a document round-trips to byte-identical text") {
    const Document original = richDocument();
    const std::string first = serialize(original);

    Document loaded;
    const LoadResult result = deserialize(first, loaded);
    REQUIRE_MESSAGE(result.ok, result.error, " at line ", result.line);

    CHECK(serialize(loaded) == first);
    CHECK(loaded == original);
}

TEST_CASE("serialization is byte-stable across repeated runs") {
    // No hash-map iteration reaches the output, so the bytes are a function of
    // the document alone.
    const Document doc = richDocument();
    const std::string a = serialize(doc);
    for(int i = 0; i < 8; i++) CHECK(serialize(doc) == a);
}

TEST_CASE("records come out in id order") {
    // Readable diffs and sane merges both rest on this.
    Document doc;
    for(int i = 0; i < 5; i++) addPoint(doc, i, i);
    const std::string text = serialize(doc);

    size_t previous = 0;
    for(int i = 1; i <= 5; i++) {
        const size_t at = text.find("entity " + std::to_string(i) + " ");
        REQUIRE(at != std::string::npos);
        CHECK(at > previous);
        previous = at;
    }
}

TEST_CASE("doubles survive the round trip exactly") {
    // Display rounding lives at the presentation boundary and never reaches
    // storage; the file is the record.
    Document doc;
    const double awkward[] = {1.0 / 3.0, 1e-17, -2.5e300, 0.1 + 0.2, 123456789.123456789};
    for(double v : awkward) addPoint(doc, v, -v);

    Document loaded;
    REQUIRE(deserialize(serialize(doc), loaded).ok);

    const auto &before = doc.entities().records();
    const auto &after = loaded.entities().records();
    REQUIRE(before.size() == after.size());
    for(size_t i = 0; i < before.size(); i++) {
        CHECK(before[i].seeds[0] == after[i].seeds[0]);
        CHECK(before[i].seeds[1] == after[i].seeds[1]);
    }
}

TEST_CASE("id watermarks survive, so a reopened document never reissues an id") {
    Document doc;
    const EntityId first = addPoint(doc, 0.0, 0.0);
    addPoint(doc, 1.0, 1.0);
    REQUIRE(doc.apply(RemoveRecord<EntityRecord>{first}).ok());

    Document loaded;
    REQUIRE(deserialize(serialize(doc), loaded).ok);
    // The deleted id must not come back on the other side of a save.
    CHECK(loaded.entities().allocator().next() == doc.entities().allocator().next());
    const auto fresh = loaded.apply(AddRecord<EntityRecord>{EntityRecord{}});
    CHECK(EntityId(fresh.allocated) != first);
}

TEST_CASE("unknown record kinds survive a round trip") {
    // A newer file opened in an older build must not shed what the old build
    // cannot read, or every save from an older install is a silent truncation.
    std::string text = serialize(richDocument());
    text += "keyframe 1 track=7 at=0.5 value=12\n";
    text += "constellation 4 shape=spiral\n";

    Document loaded;
    const LoadResult result = deserialize(text, loaded);
    REQUIRE_MESSAGE(result.ok, result.error);
    REQUIRE(loaded.unknownRecords().size() == 2);
    CHECK(loaded.unknownRecords()[0] == "keyframe 1 track=7 at=0.5 value=12");

    const std::string written = serialize(loaded);
    CHECK(written.find("keyframe 1 track=7 at=0.5 value=12\n") != std::string::npos);
    CHECK(written.find("constellation 4 shape=spiral\n") != std::string::npos);

    // And they keep surviving: the text reaches a fixed point after one cycle.
    Document again;
    REQUIRE(deserialize(written, again).ok);
    CHECK(serialize(again) == written);
}

TEST_CASE("a newer format version is refused rather than half-read") {
    Document loaded;
    const LoadResult result = deserialize("paroculus 99\n", loaded);
    CHECK_FALSE(result.ok);
    CHECK(result.line == 1);
}

TEST_CASE("a corrupt document is refused and leaves the target empty") {
    // A partially loaded document is a corrupt one wearing a valid document's
    // interface.
    Document target;
    addPoint(target, 7.0, 7.0);

    struct Case { const char *text; const char *why; };
    const Case cases[] = {
        {"", "empty"},
        {"notparoculus 0\n", "wrong magic"},
        {"paroculus 0\nentity 1 kind=hypercube role=normal layer=0 points=- seeds=0,0\n",
         "unknown entity kind"},
        {"paroculus 0\nentity 1 kind=point role=normal layer=0 points=- seeds=0,0\n"
         "constraint 1 kind=parallel driving=1 operands=1,1\n",
         "signature the taxonomy rejects"},
        {"paroculus 0\nconstraint 1 kind=horizontal driving=1 operands=404\n",
         "operand that does not exist"},
        {"paroculus 0\nentity 1 kind=segment role=normal layer=0 points=1 seeds=-\n",
         "point count that does not match the kind"},
        {"paroculus 0\nentity 1 kind=point role=normal layer=0 points=- seeds=0,0\n"
         "entity 1 kind=point role=normal layer=0 points=- seeds=1,1\n",
         "duplicate id"},
        {"paroculus 0\nparameter 1 name=\"a\" value=[p1]\n", "self-referencing parameter"},
    };

    for(const Case &c : cases) {
        Document out = target;
        const LoadResult result = deserialize(c.text, out);
        CHECK_MESSAGE(!result.ok, c.why);
        // The caller's document is untouched on failure.
        CHECK_MESSAGE(out == target, c.why);
    }
}

TEST_CASE("expression slots round-trip with their structure intact") {
    Document doc;
    ParameterRecord a;
    a.name = "a";
    a.value = Slot(2.0);
    const ParameterId aid(doc.apply(AddRecord<ParameterRecord>{a}).allocated);

    ParameterRecord expr;
    expr.name = "expr";
    // ((a * 3) - -(a / 4))
    expr.value = Slot::binary(
        ExprOp::Subtract, Slot::binary(ExprOp::Multiply, Slot::parameter(aid), Slot(3.0)),
        Slot::negate(Slot::binary(ExprOp::Divide, Slot::parameter(aid), Slot(4.0))));
    const ParameterId eid(doc.apply(AddRecord<ParameterRecord>{expr}).allocated);

    Document loaded;
    REQUIRE(deserialize(serialize(doc), loaded).ok);
    CHECK(loaded.parameters().find(eid)->value == doc.parameters().find(eid)->value);
    CHECK(*loaded.evaluate(Slot::parameter(eid)) == doctest::Approx(6.5));
}

TEST_CASE("names with spaces and quotes survive") {
    Document doc;
    LayerRecord layer;
    layer.name = "a \"tricky\" name with spaces \\ and a backslash";
    const LayerId id(doc.apply(AddRecord<LayerRecord>{layer}).allocated);

    Document loaded;
    REQUIRE(deserialize(serialize(doc), loaded).ok);
    CHECK(loaded.layers().find(id)->name == layer.name);
}

TEST_CASE("a nullable reference axis costs the format nothing until it is used") {
    // Horizontal gained a second, optional operand. A constraint that does not
    // name one has to serialize exactly as it did before, or every file and
    // every corpus entry changes under a slot nobody used.
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 100.0, 0.0);
    const EntityId subject = addSegment(doc, p, q);
    const ConstraintId plain = addConstraint(doc, ConstraintKind::Horizontal, {subject});
    REQUIRE(plain.valid());

    const std::string text = serialize(doc);
    CHECK(text.find("kind=horizontal driving=1 operands=" +
                    std::to_string(subject.value()) + "\n") != std::string::npos);

    Document loaded;
    REQUIRE(deserialize(text, loaded).ok);
    CHECK(loaded == doc);
    CHECK(serialize(loaded) == text);
}

TEST_CASE("a named reference axis round-trips") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 100.0, 0.0);
    const EntityId subject = addSegment(doc, p, q);
    const EntityId r = addPoint(doc, 0.0, 50.0);
    const EntityId s = addPoint(doc, 100.0, 110.0);
    const EntityId axis = addSegment(doc, r, s);
    const ConstraintId id = addConstraint(doc, ConstraintKind::Horizontal, {subject, axis});
    REQUIRE(id.valid());

    const std::string text = serialize(doc);
    CHECK(text.find("operands=" + std::to_string(subject.value()) + "," +
                    std::to_string(axis.value())) != std::string::npos);

    Document loaded;
    REQUIRE(deserialize(text, loaded).ok);
    CHECK(loaded == doc);
    CHECK(serialize(loaded) == text);
    CHECK(loaded.constraints().find(id)->operands[1] == axis);
}

TEST_CASE("a file naming more operands than a kind can hold is refused") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 100.0, 0.0);
    const EntityId subject = addSegment(doc, p, q);
    REQUIRE(addConstraint(doc, ConstraintKind::Horizontal, {subject}).valid());

    const std::string text = serialize(doc);
    const std::string one = "operands=" + std::to_string(subject.value());
    const size_t at = text.find(one);
    REQUIRE(at != std::string::npos);
    // Three operands where the kind holds at most two.
    const std::string tampered = text.substr(0, at) + one + ",1,1" +
                                 text.substr(at + one.size());

    Document loaded;
    CHECK_FALSE(deserialize(tampered, loaded).ok);
}

TEST_CASE("the tangent form round-trips, and costs nothing when it is the default") {
    Document doc;
    const EntityId p = addPoint(doc, 0.0, 0.0);
    const EntityId q = addPoint(doc, 100.0, 0.0);
    const EntityId segment = addSegment(doc, p, q);
    const EntityId centre = addPoint(doc, 0.0, 200.0);
    const EntityId arcStart = addPoint(doc, 50.0, 200.0);
    const EntityId arcEnd = addPoint(doc, 0.0, 250.0);
    EntityRecord a;
    a.kind = EntityKind::Arc;
    a.points = {centre, arcStart, arcEnd};
    const EntityId arc(doc.apply(AddRecord<EntityRecord>{a}).allocated);

    ConstraintRecord atStart;
    atStart.kind = ConstraintKind::Tangent;
    atStart.operands[0] = arc;
    atStart.operands[1] = segment;
    const ConstraintId first(doc.apply(AddRecord<ConstraintRecord>{atStart}).allocated);

    ConstraintRecord atEnd = atStart;
    atEnd.alternative = 1;
    const ConstraintId second(doc.apply(AddRecord<ConstraintRecord>{atEnd}).allocated);

    const std::string text = serialize(doc);
    // The default form says nothing, so every kind that has no alternatives
    // writes the line it always wrote.
    CHECK(text.find(" alt=1") != std::string::npos);
    CHECK(text.find(" alt=0") == std::string::npos);

    Document loaded;
    REQUIRE(deserialize(text, loaded).ok);
    CHECK(loaded == doc);
    CHECK(serialize(loaded) == text);
    CHECK(loaded.constraints().find(first)->alternative == 0);
    CHECK(loaded.constraints().find(second)->alternative == 1);
}

TEST_CASE("a name cannot split its own record") {
    // A newline in a name would end the line the loader is reading, so the tail
    // would arrive as a record kind nothing recognises and be kept verbatim as
    // an unknown one: the name silently truncated, the file silently grown.
    // Names are arbitrary strings through the command layer, so this is
    // reachable rather than theoretical.
    const std::string awkward =
        std::string("line one\nline two\ttabbed\r\nand a bell\a end") + '\0' + "past a null";

    Document doc;
    LayerRecord layer;
    layer.name = awkward;
    const LayerId lid(doc.apply(AddRecord<LayerRecord>{layer}).allocated);
    StyleRecord style;
    style.name = awkward;
    const StyleId sid(doc.apply(AddRecord<StyleRecord>{style}).allocated);
    GroupRecord group;
    group.name = awkward;
    const GroupId gid(doc.apply(AddRecord<GroupRecord>{group}).allocated);
    ParameterRecord parameter;
    parameter.name = awkward;
    parameter.value = Slot(1.0);
    const ParameterId pid(doc.apply(AddRecord<ParameterRecord>{parameter}).allocated);

    const std::string text = serialize(doc);
    // One line per record and nothing else: the count is what a split name
    // would change, and it changes before any name comparison would notice.
    CHECK(std::count(text.begin(), text.end(), '\n') == 6);  // version, watermark, four records

    Document loaded;
    REQUIRE(deserialize(text, loaded).ok);
    CHECK(loaded.layers().find(lid)->name == awkward);
    CHECK(loaded.styles().find(sid)->name == awkward);
    CHECK(loaded.groups().find(gid)->name == awkward);
    CHECK(loaded.parameters().find(pid)->name == awkward);
    CHECK(loaded.unknownRecords().empty());
    CHECK(loaded == doc);
}

TEST_CASE("a name that looks like an escape is not one") {
    // quote() doubles the backslash it writes, so a name holding the four
    // literal characters of a hex escape comes back as those four characters.
    Document doc;
    LayerRecord layer;
    layer.name = "\\x41 is not A, and \\\\x41 is not either";
    const LayerId id(doc.apply(AddRecord<LayerRecord>{layer}).allocated);

    Document loaded;
    REQUIRE(deserialize(serialize(doc), loaded).ok);
    CHECK(loaded.layers().find(id)->name == layer.name);
}

TEST_CASE("the decimal point is a point, whatever the process locale says") {
    // QGuiApplication calls setlocale(LC_ALL, "") on Unix, so the app runs in
    // the user's locale while the parser is locale-fixed to '.'. A printf-family
    // writer would put a comma in the file on half of Europe and every save
    // would be unreadable by the loader that wrote it.
    Document doc;
    addPoint(doc, 1.5, -0.25);
    const std::string expected = serialize(doc);
    CHECK(expected.find("1.5") != std::string::npos);

    const char *comma[] = {"de_DE.UTF-8", "fr_FR.UTF-8", "de_DE", "fr_FR"};
    const char *previous = std::setlocale(LC_ALL, nullptr);
    const std::string restore = previous ? previous : "C";

    bool tried = false;
    for(const char *name : comma) {
        if(std::setlocale(LC_ALL, name) == nullptr) continue;
        tried = true;
        CHECK(serialize(doc) == expected);

        Document loaded;
        CHECK(deserialize(expected, loaded).ok);
        CHECK(loaded == doc);
        break;
    }
    std::setlocale(LC_ALL, restore.c_str());
    // No comma-decimal locale is generated on this machine; the substring check
    // above still holds the property, weakly.
    MESSAGE("comma-decimal locale exercised: ", tried);
}

TEST_CASE("a file whose slot names a parameter that is not in it is refused") {
    // The alternative is a document that loads with a slot evaluating to
    // nullopt, which translation turns into a dimension driving to zero. Each
    // referring record kind is checked on its own, so one loop covering for
    // another cannot pass for coverage.
    enum Referrer { Style, Parameter, Constraint };
    for(Referrer who : {Style, Parameter, Constraint}) {
        CAPTURE(int(who));
        Document doc;
        ParameterRecord width;
        width.name = "width";
        width.value = Slot(40.0);
        const ParameterId wid(doc.apply(AddRecord<ParameterRecord>{width}).allocated);

        if(who == Style) {
            StyleRecord style;
            style.name = "hairline";
            style.strokeWidth = Slot::parameter(wid);
            REQUIRE(doc.apply(AddRecord<StyleRecord>{style}).ok());
        } else if(who == Parameter) {
            ParameterRecord half;
            half.name = "half";
            half.value = Slot::binary(ExprOp::Divide, Slot::parameter(wid), Slot(2.0));
            REQUIRE(doc.apply(AddRecord<ParameterRecord>{half}).ok());
        } else {
            const EntityId p = addPoint(doc, 0.0, 0.0);
            const EntityId q = addPoint(doc, 40.0, 0.0);
            REQUIRE(addConstraint(doc, ConstraintKind::PointPointDistance, {p, q},
                                  Slot::parameter(wid))
                        .valid());
        }

        const std::string text = serialize(doc);
        Document loaded;
        REQUIRE(deserialize(text, loaded).ok);

        // Drop the parameter line by hand, as a bad merge or an older build
        // shedding a record it did not understand would.
        const size_t at = text.find("parameter " + std::to_string(wid.value()) + " ");
        REQUIRE(at != std::string::npos);
        const std::string cut = text.substr(0, at) + text.substr(text.find('\n', at) + 1);

        Document broken;
        CHECK_FALSE(deserialize(cut, broken).ok);
    }
}

TEST_CASE("property: round-tripping reaches a fixed point on random documents") {
    for(uint64_t seed = 1; seed <= 30; seed++) {
        Rng rng(seed * 32749u);
        Document doc;
        std::vector<EntityId> points, segments;

        for(int i = 0; i < 30; i++) {
            const uint32_t choice = rng.below(8);
            if(choice < 3 || points.size() < 2) {
                const EntityId p = addPoint(doc, rng.real(-1e3, 1e3), rng.real(-1e3, 1e3));
                if(p.valid()) points.push_back(p);
            } else if(choice < 5) {
                const EntityId a = points[rng.below(points.size())];
                const EntityId b = points[rng.below(points.size())];
                if(a != b) {
                    const EntityId s = addSegment(doc, a, b);
                    if(s.valid()) segments.push_back(s);
                }
            } else if(choice < 6 && !segments.empty()) {
                addConstraint(doc, ConstraintKind::Horizontal,
                              {segments[rng.below(segments.size())]});
            } else if(choice < 7) {
                const EntityId a = points[rng.below(points.size())];
                const EntityId b = points[rng.below(points.size())];
                if(a != b) {
                    addConstraint(doc, ConstraintKind::PointPointDistance, {a, b},
                                  Slot(rng.real(1.0, 100.0)));
                }
            } else if(!points.empty()) {
                for(const Command &c :
                    deletionStep(doc, points[rng.below(points.size())])) {
                    REQUIRE(doc.apply(c).ok());
                }
                points.clear();
                segments.clear();
                for(const EntityRecord &e : doc.entities().records()) {
                    if(e.kind == EntityKind::Point) points.push_back(e.id);
                    if(e.kind == EntityKind::Segment) segments.push_back(e.id);
                }
            }
        }

        const std::string once = serialize(doc);
        Document loaded;
        const LoadResult result = deserialize(once, loaded);
        REQUIRE_MESSAGE(result.ok, "seed ", seed, ": ", result.error);
        CHECK_MESSAGE(serialize(loaded) == once, "seed ", seed);
        CHECK_MESSAGE(loaded == doc, "seed ", seed);
    }
}

// A random valid mutation drawn from the whole record vocabulary, so the fuzz
// below exercises curves, radii, reference constraints and deletions rather
// than the points-segments-distances the property above stays inside.
namespace {

void fuzzStep(Document &doc, Rng &rng, std::vector<EntityId> &points,
              std::vector<EntityId> &segments, std::vector<EntityId> &circles) {
    switch(rng.below(9)) {
        case 0:
        case 1: {
            const EntityId p = addPoint(doc, rng.real(-1e3, 1e3), rng.real(-1e3, 1e3));
            if(p.valid()) points.push_back(p);
            break;
        }
        case 2: {
            if(points.size() < 2) break;
            const EntityId a = points[rng.below(points.size())];
            const EntityId b = points[rng.below(points.size())];
            if(a != b) {
                const EntityId s = addSegment(doc, a, b);
                if(s.valid()) segments.push_back(s);
            }
            break;
        }
        case 3: {
            if(points.empty()) break;
            const EntityId c = addCircle(doc, points[rng.below(points.size())], rng.real(1.0, 50.0));
            if(c.valid()) circles.push_back(c);
            break;
        }
        case 4: {
            if(points.size() < 3) break;
            addArc(doc, points[rng.below(points.size())], points[rng.below(points.size())],
                   points[rng.below(points.size())]);
            break;
        }
        case 5: {
            if(segments.empty()) break;
            addConstraint(doc, rng.chance(2) ? ConstraintKind::Horizontal : ConstraintKind::Vertical,
                          {segments[rng.below(segments.size())]});
            break;
        }
        case 6: {
            if(circles.empty()) break;
            addConstraint(doc, ConstraintKind::Radius, {circles[rng.below(circles.size())]},
                          Slot(rng.real(1.0, 50.0)));
            break;
        }
        case 7: {
            if(points.size() < 2) break;
            const EntityId a = points[rng.below(points.size())];
            const EntityId b = points[rng.below(points.size())];
            if(a == b) break;
            // A reference measurement half the time, so both driving states are
            // reached by the fuzz rather than only the imposed one.
            ConstraintRecord c;
            c.kind = ConstraintKind::PointPointDistance;
            c.operands[0] = a;
            c.operands[1] = b;
            c.driving = !rng.chance(2);
            c.value = Slot(rng.real(1.0, 100.0));
            doc.apply(AddRecord<ConstraintRecord>{c});
            break;
        }
        default: {
            if(points.empty()) break;
            for(const Command &cmd : deletionStep(doc, points[rng.below(points.size())])) {
                REQUIRE(doc.apply(cmd).ok());
            }
            points.clear();
            segments.clear();
            circles.clear();
            for(const EntityRecord &e : doc.entities().records()) {
                if(e.kind == EntityKind::Point) points.push_back(e.id);
                if(e.kind == EntityKind::Segment) segments.push_back(e.id);
                if(e.kind == EntityKind::Circle) circles.push_back(e.id);
            }
            break;
        }
    }
}

}  // namespace

TEST_CASE("fuzz: broad random documents reach a byte fixed point after one cycle") {
    // The property the freeze rests on, at volume and over the whole vocabulary:
    // whatever a random valid edit script builds, serialize then load then
    // serialize is the identity from the first cycle on. Seeded from the rich
    // fixture half the time, so regions, tags, groups, styles and composites ride
    // through the same churn the loose geometry does.
    for(uint64_t seed = 1; seed <= 80; seed++) {
        Rng rng(seed * 6364136223846793005ull + 1442695040888963407ull);
        Document doc = (seed % 2 == 0) ? richDocument() : Document();

        std::vector<EntityId> points, segments, circles;
        for(const EntityRecord &e : doc.entities().records()) {
            if(e.kind == EntityKind::Point) points.push_back(e.id);
            if(e.kind == EntityKind::Segment) segments.push_back(e.id);
            if(e.kind == EntityKind::Circle) circles.push_back(e.id);
        }

        const int steps = 20 + static_cast<int>(rng.below(40));
        for(int i = 0; i < steps; i++) fuzzStep(doc, rng, points, segments, circles);

        const std::string once = serialize(doc);
        Document loaded;
        const LoadResult first = deserialize(once, loaded);
        REQUIRE_MESSAGE(first.ok, "seed ", seed, ": ", first.error);
        const std::string twice = serialize(loaded);
        CHECK_MESSAGE(twice == once, "seed ", seed);       // byte fixed point
        CHECK_MESSAGE(loaded == doc, "seed ", seed);       // and the model round-trips

        // The document the loader built is itself byte-stable — a second load of
        // the same text lands on the same bytes, which is the property a diff
        // and a merge both depend on.
        Document again;
        REQUIRE(deserialize(twice, again).ok);
        CHECK_MESSAGE(serialize(again) == twice, "seed ", seed);
    }
}

TEST_CASE("forward-compat: a future-versioned file sheds nothing through open and save") {
    // tests/corpus/future.paro is a version-0 document carrying record kinds no
    // build in this tree defines — keyframe, animation, constellation, mesh —
    // which is what a future additive version looks like to a reader that
    // predates it. The freeze's whole forward-compatibility promise is that
    // opening and re-saving it loses none of them.
    const std::string path = std::string(PAROCULUS_CORPUS_DIR) + "/future.paro";
    std::ifstream file(path);
    REQUIRE_MESSAGE(file.good(), "missing corpus file: ", path);
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string original = buffer.str();

    Document loaded;
    const LoadResult result = deserialize(original, loaded);
    REQUIRE_MESSAGE(result.ok, result.error);

    // The known records arrived: two segments, a horizontal, a driving distance.
    CHECK(loaded.entities().size() == 5);
    CHECK(loaded.constraints().size() == 2);

    // The unknown ones arrived verbatim and in order.
    REQUIRE(loaded.unknownRecords().size() == 4);
    CHECK(loaded.unknownRecords()[0] == "keyframe 1 track=7 at=0.5 value=12");
    CHECK(loaded.unknownRecords()[3] == "mesh 9 verts=42 faces=80");

    // Re-saving keeps every future line, and a second cycle is the identity —
    // an older build in the loop cannot erode a newer file one save at a time.
    const std::string written = serialize(loaded);
    for(const std::string &line : loaded.unknownRecords()) {
        CHECK_MESSAGE(written.find(line + "\n") != std::string::npos, line);
    }
    Document again;
    REQUIRE(deserialize(written, again).ok);
    CHECK(serialize(again) == written);
    CHECK(again.unknownRecords().size() == 4);
}
