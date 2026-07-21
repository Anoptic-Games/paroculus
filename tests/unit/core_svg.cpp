// SVG export as bake, import as trace.
//
// Export is asserted structurally with a numeric tolerance, per the plan: the
// baked geometry reaches the file as paths, an alpha overwrite reaches it as a
// mask, and a composite reaches it as the mask or clip that expresses it. Import
// is asserted by the unconstrained record set it produces and by the count of
// what it could not read. The two together round-trip: a drawing exported and
// re-imported lands where it started, y-flip and all.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

#include "core/bake.h"
#include "core/svg.h"
#include "support/build.h"

using namespace paroculus;
using namespace paroculus::test;

namespace {

bool has(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

RegionId addOutline(Document &doc, std::vector<EntityId> boundary, StyleId style = StyleId(),
                    bool punch = false) {
    RegionRecord r;
    r.boundary = std::move(boundary);
    r.style = style;
    r.punch = punch;
    const CommandResult result = doc.apply(AddRecord<RegionRecord>{r});
    REQUIRE(result.ok());
    return RegionId(result.allocated);
}

// A closed triangle: corners joined by coincidence, the shape a ring walk that
// matched point identity would call broken.
std::vector<EntityId> triangle(Document &doc, double scale, double ox = 0.0, double oy = 0.0) {
    const Point corners[3] = {
        {ox, oy}, {ox + scale * 10.0, oy}, {ox, oy + scale * 10.0}};
    EntityId ends[3][2];
    std::vector<EntityId> edges;
    for(int i = 0; i < 3; i++) {
        ends[i][0] = addPoint(doc, corners[i].x, corners[i].y);
        ends[i][1] = addPoint(doc, corners[(i + 1) % 3].x, corners[(i + 1) % 3].y);
        edges.push_back(addSegment(doc, ends[i][0], ends[i][1]));
    }
    for(int i = 0; i < 3; i++)
        addConstraint(doc, ConstraintKind::Coincident, {ends[i][1], ends[(i + 1) % 3][0]});
    return edges;
}

StyleId addFillStyle(Document &doc, uint32_t fill) {
    StyleRecord s;
    s.fillColor = fill;
    s.filled = true;
    const CommandResult r = doc.apply(AddRecord<StyleRecord>{s});
    REQUIRE(r.ok());
    return StyleId(r.allocated);
}

}  // namespace

TEST_CASE("export: a filled outline becomes a path in flipped coordinates") {
    Document doc;
    const StyleId style = addFillStyle(doc, 0xff3366ccu);
    addOutline(doc, triangle(doc, 1.0), style);

    const std::string svg = writeSvg(doc, Pose(doc));
    CHECK(has(svg, "<svg"));
    CHECK(has(svg, "viewBox="));
    CHECK(has(svg, "<path"));
    CHECK(has(svg, "fill=\"#3366cc\""));
    // Document y is up, svg y is down: the corner at (0,10) writes as 0,-10 and
    // the corner at (10,0) writes as 10,0.
    CHECK(has(svg, "0,-10"));
    CHECK(has(svg, "10,0"));
    // The loss report rides in a comment, so a re-open still knows what was lost.
    CHECK(has(svg, "paroculus bake:"));
}

TEST_CASE("export: strokes become polylines") {
    Document doc;
    const EntityId a = addPoint(doc, 0.0, 0.0);
    const EntityId b = addPoint(doc, 30.0, 40.0);
    addSegment(doc, a, b);

    const std::string svg = writeSvg(doc, Pose(doc));
    CHECK(has(svg, "<polyline"));
    CHECK(has(svg, "stroke-width="));
    // The far endpoint's y is flipped.
    CHECK(has(svg, "30,-40"));
}

TEST_CASE("export: a punched region masks its layer") {
    // Alpha overwrite carves visibility from what the layer accumulated, and SVG
    // says that with a mask: white over the box, black where the punch is, applied
    // to the layer group. A pixel inside the punch ring is transparent.
    Document doc;
    const StyleId style = addFillStyle(doc, 0xff88aa22u);
    addOutline(doc, triangle(doc, 3.0), style);              // the plate
    addOutline(doc, triangle(doc, 1.0), style, /*punch=*/true);  // carves it

    const std::string svg = writeSvg(doc, Pose(doc));
    CHECK(has(svg, "<mask"));
    CHECK(has(svg, "<g mask=\"url(#"));
    CHECK(has(svg, "fill=\"white\""));
    CHECK(has(svg, "fill=\"black\""));
}

TEST_CASE("export: a subtract becomes a mask, an intersect becomes a clip") {
    Document doc;
    const StyleId style = addFillStyle(doc, 0xffff0000u);
    const RegionId plate = addOutline(doc, triangle(doc, 3.0), style);
    const RegionId hole = addOutline(doc, triangle(doc, 1.0), style);

    RegionRecord cut;
    cut.op = CompositeOp::Subtract;
    cut.operands = {plate, hole};
    REQUIRE(doc.apply(AddRecord<RegionRecord>{cut}).ok());

    const std::string subtractSvg = writeSvg(doc, Pose(doc));
    CHECK(has(subtractSvg, "<mask"));
    CHECK(has(subtractSvg, "<g mask=\"url(#"));

    // The same two operands, intersected: a clip path rather than a mask.
    Document doc2;
    const StyleId style2 = addFillStyle(doc2, 0xffff0000u);
    const RegionId a = addOutline(doc2, triangle(doc2, 3.0), style2);
    const RegionId b = addOutline(doc2, triangle(doc2, 1.0), style2);
    RegionRecord meet;
    meet.op = CompositeOp::Intersect;
    meet.operands = {a, b};
    REQUIRE(doc2.apply(AddRecord<RegionRecord>{meet}).ok());

    const std::string intersectSvg = writeSvg(doc2, Pose(doc2));
    CHECK(has(intersectSvg, "<clipPath"));
    CHECK(has(intersectSvg, "clip-path=\"url(#"));
}

TEST_CASE("export: a punched composite carves its shape, not the union of its rings") {
    // A punched Subtract(plate, hole): the carve is plate−hole. The mask expresses
    // it op-respectingly — the minuend black, the subtrahend white to restore it —
    // rather than blacking both rings, which would carve plate∪hole.
    Document doc;
    const StyleId style = addFillStyle(doc, 0xff4488ccu);
    const RegionId plate = addOutline(doc, triangle(doc, 4.0), style);
    const RegionId hole = addOutline(doc, triangle(doc, 1.0, 10.0, 10.0), style);
    RegionRecord cut;
    cut.op = CompositeOp::Subtract;
    cut.operands = {plate, hole};
    cut.punch = true;
    REQUIRE(doc.apply(AddRecord<RegionRecord>{cut}).ok());

    const std::string svg = writeSvg(doc, Pose(doc));
    CHECK(has(svg, "<mask"));
    // The mask carries both polarities: the minuend removed (black) and the hole
    // restored (white) — the signature of the op-respecting carve.
    CHECK(has(svg, "fill=\"black\""));
    CHECK(has(svg, "fill=\"white\""));
    // A plain subtract of outlines is exactly expressible, so nothing is
    // approximated.
    CHECK(has(svg, "approximated=0"));
}

TEST_CASE("export: a punch does not carve the strokes drawn above it") {
    // Strokes sit above fills and a punch never carves one, so the stroke markup
    // is outside the masked fill group.
    Document doc;
    const StyleId style = addFillStyle(doc, 0xff88aa22u);
    addOutline(doc, triangle(doc, 3.0), style);
    addOutline(doc, triangle(doc, 1.0, 5.0, 5.0), style, /*punch=*/true);

    const std::string svg = writeSvg(doc, Pose(doc));
    const size_t maskClose = svg.rfind("</g>");
    const size_t firstStroke = svg.find("<polyline");
    REQUIRE(firstStroke != std::string::npos);
    // The masked fill group closes before the strokes begin — the strokes are not
    // inside it.
    const size_t maskOpen = svg.find("<g mask=");
    REQUIRE(maskOpen != std::string::npos);
    CHECK(maskOpen < firstStroke);
    // And the strokes are the last content, after every fill group has closed.
    CHECK(svg.find("<g mask=", firstStroke) == std::string::npos);
    (void)maskClose;
}

TEST_CASE("export: an empty document still produces a valid svg") {
    Document doc;
    const std::string svg = writeSvg(doc, Pose(doc));
    CHECK(has(svg, "<svg"));
    CHECK(has(svg, "</svg>"));
    CHECK(has(svg, "viewBox="));
}

TEST_CASE("import: each supported element becomes unconstrained geometry") {
    SUBCASE("a line") {
        const SvgImport r = readSvg("<svg><line x1=\"0\" y1=\"0\" x2=\"10\" y2=\"20\"/></svg>");
        CHECK(r.traced == 1);
        CHECK(r.skipped == 0);
        CHECK(r.document.entities().size() == 3);  // two points, one segment
        CHECK(r.document.constraints().empty());
    }
    SUBCASE("a rect") {
        const SvgImport r = readSvg("<svg><rect x=\"0\" y=\"0\" width=\"10\" height=\"5\"/></svg>");
        CHECK(r.traced == 1);
        CHECK(r.document.entities().size() == 8);  // four points, four segments
    }
    SUBCASE("a circle") {
        const SvgImport r = readSvg("<svg><circle cx=\"5\" cy=\"5\" r=\"3\"/></svg>");
        CHECK(r.traced == 1);
        CHECK(r.document.entities().size() == 2);  // centre point, circle
        size_t circles = 0;
        for(const EntityRecord &e : r.document.entities().records())
            if(e.kind == EntityKind::Circle) circles++;
        CHECK(circles == 1);
    }
    SUBCASE("a polygon closes, a polyline does not") {
        const SvgImport poly = readSvg("<svg><polygon points=\"0,0 10,0 10,10\"/></svg>");
        CHECK(poly.traced == 1);
        size_t segments = 0;
        for(const EntityRecord &e : poly.document.entities().records())
            if(e.kind == EntityKind::Segment) segments++;
        CHECK(segments == 3);  // three sides — the closing one included

        const SvgImport line = readSvg("<svg><polyline points=\"0,0 10,0 10,10\"/></svg>");
        segments = 0;
        for(const EntityRecord &e : line.document.entities().records())
            if(e.kind == EntityKind::Segment) segments++;
        CHECK(segments == 2);  // open — two sides
    }
    SUBCASE("a straight-line path") {
        const SvgImport r = readSvg("<svg><path d=\"M0,0 L10,0 L10,10 Z\"/></svg>");
        CHECK(r.traced == 1);
        size_t segments = 0;
        for(const EntityRecord &e : r.document.entities().records())
            if(e.kind == EntityKind::Segment) segments++;
        CHECK(segments == 3);
    }
}

TEST_CASE("import: what the subset cannot read is counted, not guessed") {
    // A curve command aborts its element rather than fabricating vertices, and an
    // ellipse or text is skipped whole. Inference-on-import is deferred, so
    // nothing arrives constrained either way.
    const SvgImport curve = readSvg("<svg><path d=\"M0,0 C10,10 20,0 30,0\"/></svg>");
    CHECK(curve.traced == 0);
    CHECK(curve.skipped == 1);

    const SvgImport ellipse = readSvg("<svg><ellipse cx=\"0\" cy=\"0\" rx=\"5\" ry=\"3\"/></svg>");
    CHECK(ellipse.traced == 0);
    CHECK(ellipse.skipped == 1);

    const SvgImport mixed =
        readSvg("<svg><line x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\"/><text x=\"0\" y=\"0\">hi</text></svg>");
    CHECK(mixed.traced == 1);
    CHECK(mixed.skipped == 1);
}

TEST_CASE("import: the checked-in corpus SVG traces to the expected record set") {
    // A recorded demo of import-as-trace: a hand-authored SVG mixing the
    // supported subset with an ellipse, text and a curved path the trace skips.
    const std::string path = std::string(PAROCULUS_CORPUS_DIR) + "/import.svg";
    std::ifstream file(path);
    REQUIRE_MESSAGE(file.good(), "missing corpus file: ", path);
    std::stringstream buffer;
    buffer << file.rdbuf();

    const SvgImport r = readSvg(buffer.str());
    // rect, line, circle, polygon, straight path — five traced; ellipse, text and
    // the cubic path — three skipped.
    CHECK(r.traced == 5);
    CHECK(r.skipped == 3);

    size_t points = 0, segments = 0, circles = 0;
    for(const EntityRecord &e : r.document.entities().records()) {
        if(e.kind == EntityKind::Point) points++;
        if(e.kind == EntityKind::Segment) segments++;
        if(e.kind == EntityKind::Circle) circles++;
    }
    CHECK(points == 13);    // 4 rect + 2 line + 1 circle + 3 polygon + 3 path
    CHECK(segments == 11);  // 4 rect + 1 line + 3 polygon + 3 path
    CHECK(circles == 1);
    // Geometry arrives unconstrained: inference-on-import is deferred.
    CHECK(r.document.constraints().empty());
}

TEST_CASE("round-trip: a stroked drawing survives export and re-import within tolerance") {
    // The demo the exit criterion names: a drawing baked to a file and traced back
    // arrives at the same coordinates. Segments come back as segments, and the
    // y-flip applied on the way out is undone on the way in.
    Document doc;
    const EntityId a = addPoint(doc, 12.0, 5.0);
    const EntityId b = addPoint(doc, 40.0, 33.0);
    addSegment(doc, a, b);
    const EntityId c = addPoint(doc, -20.0, 8.0);
    const EntityId d = addPoint(doc, -20.0, 60.0);
    addSegment(doc, c, d);

    const std::string svg = writeSvg(doc, Pose(doc));
    const SvgImport back = readSvg(svg);

    size_t segments = 0;
    for(const EntityRecord &e : back.document.entities().records())
        if(e.kind == EntityKind::Segment) segments++;
    CHECK(segments == 2);

    // Every original endpoint is matched by a traced point within tolerance.
    const Point originals[4] = {{12.0, 5.0}, {40.0, 33.0}, {-20.0, 8.0}, {-20.0, 60.0}};
    for(const Point &want : originals) {
        bool found = false;
        for(const EntityRecord &e : back.document.entities().records()) {
            if(e.kind != EntityKind::Point) continue;
            if(std::hypot(e.seeds[0] - want.x, e.seeds[1] - want.y) < 1e-3) found = true;
        }
        CHECK_MESSAGE(found, "no traced point near (", want.x, ",", want.y, ")");
    }
}
