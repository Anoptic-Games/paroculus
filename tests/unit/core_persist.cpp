#include <doctest/doctest.h>

#include <clocale>

#include "core/persist.h"
#include "support/build.h"

using namespace paroculus;
using paroculus::test::Rng;
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

    RegionRecord region;
    region.boundary = edges;
    region.style = sid;
    region.layer = lid;
    doc.apply(AddRecord<RegionRecord>{region});

    TagRecord tag;
    tag.kind = TagKind::Rectangle;
    tag.entities = edges;
    tag.constraints = {doc.constraints().records().front().id};
    doc.apply(AddRecord<TagRecord>{tag});

    GroupRecord group;
    group.name = "frame";
    group.members = edges;
    doc.apply(AddRecord<GroupRecord>{group});

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
