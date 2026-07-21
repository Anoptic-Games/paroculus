#include "core/svg.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>

namespace paroculus {

namespace {

// ---------------------------------------------------------------------------
// Number and colour formatting
// ---------------------------------------------------------------------------

// Fixed precision through to_chars, never the printf family: SVG coordinates are
// machine-independent for the same reason document numbers are, and a "%g" under
// a comma-decimal locale would write coordinates a parser reads as pairs. Trailing
// zeros and a bare "-0" are trimmed so the output is stable and readable.
std::string num(double v, int precision) {
    // A non-finite coordinate (a degenerate pose) would otherwise write the
    // literal "nan"/"inf" into an attribute and make the file unparseable; 0 is a
    // defined, harmless fallback.
    if(!std::isfinite(v)) return "0";
    if(v == 0.0) v = 0.0;  // fold -0.0
    // Sized for the widest fixed form of a finite double (~309 integer digits
    // plus a point and `precision` fraction digits), so a huge coordinate is
    // written in full rather than silently falling back to "0" on overflow.
    std::array<char, 350> buf{};
    const std::to_chars_result r =
        std::to_chars(buf.data(), buf.data() + buf.size(), v, std::chars_format::fixed, precision);
    if(r.ec != std::errc{}) return "0";
    std::string s(buf.data(), static_cast<size_t>(r.ptr - buf.data()));
    if(s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if(!s.empty() && s.back() == '.') s.pop_back();
    }
    if(s == "-0" || s.empty()) s = "0";
    return s;
}

// Packed ARGB (SkColor byte order) to an SVG hex colour and a separate opacity,
// because SVG keeps colour and alpha in different attributes.
struct SvgColour {
    std::string hex;
    double opacity = 1.0;
};

SvgColour colourOf(uint32_t argb) {
    const unsigned a = (argb >> 24) & 0xffu;
    const unsigned r = (argb >> 16) & 0xffu;
    const unsigned g = (argb >> 8) & 0xffu;
    const unsigned b = argb & 0xffu;
    const char *digits = "0123456789abcdef";
    std::string hex = "#";
    for(unsigned c : {r, g, b}) {
        hex += digits[(c >> 4) & 0xf];
        hex += digits[c & 0xf];
    }
    return SvgColour{hex, a / 255.0};
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------

struct Bounds {
    double minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0;
    bool any = false;
    void add(const Point &p) {
        if(!any) {
            minX = maxX = p.x;
            minY = maxY = p.y;
            any = true;
            return;
        }
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
};

class SvgWriter {
public:
    explicit SvgWriter(const Bake &bake, const SvgOptions &options)
        : bake_(bake), precision_(options.precision), margin_(options.margin) {}

    std::string write() {
        computeBounds();
        indexTree();

        // Layers back to front, exactly as the bake ordered them: roots carry the
        // draw order in their index, and strokes fall in after the roots of the
        // same layer. A layer with only strokes still gets its group.
        std::vector<LayerId> layers;
        auto note = [&](LayerId l) {
            if(std::find(layers.begin(), layers.end(), l) == layers.end()) layers.push_back(l);
        };
        for(size_t g : roots_) note(rootLayer_[g]);
        for(const BakedStroke &s : bake_.strokes) note(s.layer);

        for(LayerId layer : layers) emitLayer(layer);

        return assemble();
    }

private:
    // svg y is the negation of document y; x is untouched.
    double sx(double x) const { return x; }
    double sy(double y) const { return -y; }
    std::string n(double v) const { return num(v, precision_); }

    void computeBounds() {
        for(const BakedFill &f : bake_.fills)
            for(const Point &p : f.ring) bounds_.add(p);
        for(const BakedStroke &s : bake_.strokes) {
            bounds_.add(s.from);
            bounds_.add(s.to);
        }
        if(!bounds_.any) {
            bounds_.minX = bounds_.minY = 0.0;
            bounds_.maxX = bounds_.maxY = 100.0;
        }
        // The viewBox is computed in svg space, where y is flipped, so the
        // document's top becomes the box's top.
        vbX_ = bounds_.minX - margin_;
        vbY_ = sy(bounds_.maxY) - margin_;
        vbW_ = (bounds_.maxX - bounds_.minX) + 2.0 * margin_;
        vbH_ = (bounds_.maxY - bounds_.minY) + 2.0 * margin_;
    }

    // Reunites the fills and subgroups of each composite by their shared seq, so
    // a group's operands come out in the order they were declared — which is the
    // only thing marking a subtract's minuend or an intersect's clip base.
    struct Child {
        bool isFill = false;
        size_t index = 0;  // into bake_.fills, or bake_.groups
        size_t seq = 0;
    };

    void indexTree() {
        children_.assign(bake_.groups.size(), {});
        for(size_t i = 0; i < bake_.fills.size(); i++) {
            const BakedFill &f = bake_.fills[i];
            if(f.group < children_.size()) children_[f.group].push_back({true, i, f.seq});
        }
        for(size_t g = 0; g < bake_.groups.size(); g++) {
            const BakedGroup &grp = bake_.groups[g];
            if(grp.parent == NO_BAKE_GROUP) {
                roots_.push_back(g);
            } else if(grp.parent < children_.size()) {
                children_[grp.parent].push_back({false, g, grp.seq});
            }
        }
        for(std::vector<Child> &c : children_)
            std::sort(c.begin(), c.end(), [](const Child &a, const Child &b) { return a.seq < b.seq; });

        // Each root's layer and whether it is a punch, read off its leaf fills.
        rootLayer_.assign(bake_.groups.size(), LayerId());
        rootPunch_.assign(bake_.groups.size(), false);
        for(size_t g : roots_) {
            std::vector<const BakedFill *> leaves;
            collectLeaves(g, leaves);
            if(!leaves.empty()) {
                rootLayer_[g] = leaves.front()->layer;
                rootPunch_[g] = leaves.front()->punch;
            }
        }
    }

    void collectLeaves(size_t group, std::vector<const BakedFill *> &out) const {
        if(group >= children_.size()) return;
        for(const Child &c : children_[group]) {
            if(c.isFill) out.push_back(&bake_.fills[c.index]);
            else collectLeaves(c.index, out);
        }
    }

    std::string ringData(const std::vector<Point> &ring) const {
        std::string d;
        for(size_t i = 0; i < ring.size(); i++) {
            d += (i == 0 ? "M" : "L");
            d += n(sx(ring[i].x)) + "," + n(sy(ring[i].y)) + " ";
        }
        d += "Z";
        return d;
    }

    // Draws a fill or a group's shape into a mask as black (carve) or white
    // (restore) paths, respecting the group's operator so a nested composite
    // carves the region it actually is: a subtract flips its subtrahends to the
    // opposite polarity, which on a white base is exactly minuend-minus-hole.
    // Intersect cannot be written as a flat set of mask paths, so its operands
    // past the first are dropped and the approximation is recorded rather than
    // shipped silently. Correct when operands nest (a hole inside its shape),
    // which is the ordinary case; a pathological non-nesting arrangement is where
    // the mask, like any structural boolean, can only approximate.
    void renderShapeInto(std::string &out, bool isFill, size_t index, bool carve) {
        const char *colour = carve ? "black" : "white";
        if(isFill) {
            out += "    <path d=\"" + ringData(bake_.fills[index].ring) + "\" fill=\"" + colour +
                   "\"/>\n";
            return;
        }
        if(index >= children_.size()) return;
        const std::vector<Child> &ch = children_[index];
        switch(bake_.groups[index].op) {
            case CompositeOp::Outline:
            case CompositeOp::Union:
                for(const Child &c : ch) renderShapeInto(out, c.isFill, c.index, carve);
                return;
            case CompositeOp::Subtract:
                if(ch.empty()) return;
                renderShapeInto(out, ch[0].isFill, ch[0].index, carve);
                for(size_t i = 1; i < ch.size(); i++)
                    renderShapeInto(out, ch[i].isFill, ch[i].index, !carve);
                return;
            case CompositeOp::Intersect:
                if(!ch.empty()) renderShapeInto(out, ch[0].isFill, ch[0].index, carve);
                approximated_++;
                return;
        }
    }

    // A mask over the whole viewBox carving out each shape's region — white where
    // it shows, black where a shape removes it.
    std::string defineShapeMask(const std::vector<Child> &shapes) {
        const std::string id = "m" + std::to_string(nextId_++);
        defs_ += "  <mask id=\"" + id + "\" maskUnits=\"userSpaceOnUse\" x=\"" + n(vbX_) +
                 "\" y=\"" + n(vbY_) + "\" width=\"" + n(vbW_) + "\" height=\"" + n(vbH_) + "\">\n";
        defs_ += "    <rect x=\"" + n(vbX_) + "\" y=\"" + n(vbY_) + "\" width=\"" + n(vbW_) +
                 "\" height=\"" + n(vbH_) + "\" fill=\"white\"/>\n";
        for(const Child &s : shapes) renderShapeInto(defs_, s.isFill, s.index, /*carve=*/true);
        defs_ += "  </mask>\n";
        return id;
    }

    // A clip path that is the intersection of the given operands, achieved by
    // nesting: the first operand's shape carries a clip-path to a clip of the
    // rest. SVG unions the shapes within one clipPath, so intersection has to be
    // built one nesting level at a time, and a composite operand beyond a union is
    // only approximated by the union of its leaves.
    std::string defineIntersectClip(const std::vector<Child> &operands, size_t from) {
        const std::string id = "c" + std::to_string(nextId_++);
        std::vector<const BakedFill *> leaves;
        if(operands[from].isFill) {
            leaves.push_back(&bake_.fills[operands[from].index]);
        } else {
            const CompositeOp op = bake_.groups[operands[from].index].op;
            if(op == CompositeOp::Subtract || op == CompositeOp::Intersect) approximated_++;
            collectLeaves(operands[from].index, leaves);
        }

        std::string nested;
        if(from + 1 < operands.size()) nested = defineIntersectClip(operands, from + 1);

        defs_ += "  <clipPath id=\"" + id + "\" clipPathUnits=\"userSpaceOnUse\">\n";
        for(const BakedFill *l : leaves) {
            defs_ += "    <path d=\"" + ringData(l->ring) + "\"";
            if(!nested.empty()) defs_ += " clip-path=\"url(#" + nested + ")\"";
            defs_ += "/>\n";
        }
        defs_ += "  </clipPath>\n";
        return id;
    }

    void emitFill(std::string &out, const BakedFill &f) {
        const SvgColour c = colourOf(f.colour);
        out += "    <path d=\"" + ringData(f.ring) + "\" fill=\"" + c.hex + "\"";
        if(c.opacity < 1.0) out += " fill-opacity=\"" + n(c.opacity) + "\"";
        out += "/>\n";
    }

    void emitChild(std::string &out, const Child &c) {
        if(c.isFill) emitFill(out, bake_.fills[c.index]);
        else emitGroup(out, c.index);
    }

    void emitGroup(std::string &out, size_t g) {
        if(g >= children_.size()) return;
        const CompositeOp op = bake_.groups[g].op;
        const std::vector<Child> &ch = children_[g];
        if(ch.empty()) return;

        switch(op) {
            case CompositeOp::Outline:
            case CompositeOp::Union:
                // A union is what painting the operands in place already is when
                // they share a fill; an outline group holds exactly one operand.
                for(const Child &c : ch) emitChild(out, c);
                return;
            case CompositeOp::Subtract: {
                // Operand zero is the minuend; the rest are carved out of it with a
                // mask built op-respectingly, so a composite subtrahend removes the
                // region it is rather than the union of its rings.
                const std::vector<Child> holes(ch.begin() + 1, ch.end());
                const std::string mask = holes.empty() ? "" : defineShapeMask(holes);
                if(!mask.empty()) out += "    <g mask=\"url(#" + mask + ")\">\n";
                emitChild(out, ch[0]);
                if(!mask.empty()) out += "    </g>\n";
                return;
            }
            case CompositeOp::Intersect: {
                // Operand zero, clipped by the intersection of the rest.
                if(ch.size() == 1) {
                    emitChild(out, ch[0]);
                    return;
                }
                const std::string clip = defineIntersectClip(ch, 1);
                out += "    <g clip-path=\"url(#" + clip + ")\">\n";
                emitChild(out, ch[0]);
                out += "    </g>\n";
                return;
            }
        }
    }

    void emitLayer(LayerId layer) {
        // The layer's roots in draw order, which is z-order. A punch carves what
        // its layer accumulated below it — the fills already painted, never a fill
        // above it and never a stroke — so masking is applied incrementally as the
        // punches are reached rather than over the whole layer at once.
        std::string fills;
        for(size_t g : roots_) {
            if(rootLayer_[g] != layer) continue;
            if(rootPunch_[g]) {
                const std::vector<Child> shape{Child{false, g, 0}};
                const std::string mask = defineShapeMask(shape);
                fills = "    <g mask=\"url(#" + mask + ")\">\n" + fills + "    </g>\n";
            } else {
                emitGroup(fills, g);
            }
        }

        body_ += "  <g>\n";
        body_ += fills;
        emitStrokes(body_, layer);  // strokes sit above the fills and are never carved
        body_ += "  </g>\n";
    }

    // Consecutive runs of one stroked entity chain end to end; coalescing them
    // into a polyline keeps a circle from becoming 128 separate elements.
    void emitStrokes(std::string &out, LayerId layer) {
        std::vector<const BakedStroke *> here;
        for(const BakedStroke &s : bake_.strokes)
            if(s.layer == layer) here.push_back(&s);

        size_t i = 0;
        while(i < here.size()) {
            const BakedStroke &first = *here[i];
            std::string points = n(sx(first.from.x)) + "," + n(sy(first.from.y)) + " " +
                                 n(sx(first.to.x)) + "," + n(sy(first.to.y));
            Point cursor = first.to;
            size_t j = i + 1;
            while(j < here.size() && here[j]->colour == first.colour &&
                  here[j]->width == first.width && here[j]->from.x == cursor.x &&
                  here[j]->from.y == cursor.y) {
                points += " " + n(sx(here[j]->to.x)) + "," + n(sy(here[j]->to.y));
                cursor = here[j]->to;
                j++;
            }
            const SvgColour c = colourOf(first.colour);
            out += "    <polyline points=\"" + points + "\" fill=\"none\" stroke=\"" + c.hex +
                   "\" stroke-width=\"" + n(first.width) + "\"";
            if(c.opacity < 1.0) out += " stroke-opacity=\"" + n(c.opacity) + "\"";
            out += "/>\n";
            i = j;
        }
    }

    std::string assemble() const {
        std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        out += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"" + n(vbX_) + " " + n(vbY_) +
               " " + n(vbW_) + " " + n(vbH_) + "\" width=\"" + n(vbW_) + "\" height=\"" + n(vbH_) +
               "\">\n";
        // The loss report, so a re-open of the file (which cannot see the live
        // document) still records what the bake destroyed.
        out += "  <!-- paroculus bake: constraints=" + std::to_string(bake_.constraintsDropped) +
               " parameters=" + std::to_string(bake_.parametersDropped) +
               " tags=" + std::to_string(bake_.tagsDropped) +
               " regions=" + std::to_string(bake_.regionsFlattened) +
               " broken=" + std::to_string(bake_.regionsBroken) +
               // Composites SVG masks and clips cannot express exactly — an
               // intersect inside a mask, a non-union composite operand of an
               // intersect — approximated rather than shipped as a wrong shape
               // with no word said. Zero in the ordinary case.
               " approximated=" + std::to_string(approximated_) + " -->\n";
        if(!defs_.empty()) out += "  <defs>\n" + defs_ + "  </defs>\n";
        out += body_;
        out += "</svg>\n";
        return out;
    }

    const Bake &bake_;
    int precision_;
    double margin_;
    Bounds bounds_;
    double vbX_ = 0, vbY_ = 0, vbW_ = 0, vbH_ = 0;

    std::vector<std::vector<Child>> children_;
    std::vector<size_t> roots_;
    std::vector<LayerId> rootLayer_;
    std::vector<bool> rootPunch_;

    // Composites the structural export could only approximate. Not silent: it
    // rides the loss-report comment, on the same rule the bake counts what it
    // destroyed.
    int approximated_ = 0;

    std::string defs_;
    std::string body_;
    int nextId_ = 0;
};

}  // namespace

std::string writeSvg(const Bake &bake, const SvgOptions &options) {
    SvgWriter writer(bake, options);
    return writer.write();
}

std::string writeSvg(const Document &doc, const Pose &pose, const SvgOptions &options) {
    return writeSvg(bakeForExport(doc, pose), options);
}

// ---------------------------------------------------------------------------
// Trace (import)
// ---------------------------------------------------------------------------

namespace {

// A found element: its tag name and its attributes as a flat text blob, from
// which individual attributes are pulled by name. A deliberately small parser —
// the subset is straight lines and circles, not a conforming SVG reader.
struct Element {
    std::string_view tag;
    std::string_view attrs;
    bool selfClosing = false;
};

// The value of `name="..."` within an attribute blob, or nullopt.
std::optional<std::string_view> attr(std::string_view attrs, std::string_view name) {
    size_t pos = 0;
    while(pos < attrs.size()) {
        const size_t eq = attrs.find('=', pos);
        if(eq == std::string_view::npos) break;
        // The token immediately before '=' (skipping spaces) is the attribute name.
        size_t nameEnd = eq;
        while(nameEnd > pos && (attrs[nameEnd - 1] == ' ' || attrs[nameEnd - 1] == '\n' ||
                                attrs[nameEnd - 1] == '\t' || attrs[nameEnd - 1] == '\r'))
            nameEnd--;
        size_t nameStart = nameEnd;
        while(nameStart > pos && attrs[nameStart - 1] != ' ' && attrs[nameStart - 1] != '\n' &&
              attrs[nameStart - 1] != '\t' && attrs[nameStart - 1] != '\r')
            nameStart--;
        const std::string_view found = attrs.substr(nameStart, nameEnd - nameStart);

        size_t vs = eq + 1;
        while(vs < attrs.size() && attrs[vs] != '"' && attrs[vs] != '\'') vs++;
        if(vs >= attrs.size()) break;
        const char quote = attrs[vs];
        const size_t ve = attrs.find(quote, vs + 1);
        if(ve == std::string_view::npos) break;
        if(found == name) return attrs.substr(vs + 1, ve - vs - 1);
        pos = ve + 1;
    }
    return std::nullopt;
}

std::optional<double> number(std::string_view s) {
    // Trim surrounding space and a trailing unit-ish suffix is not accepted:
    // the subset is unitless user coordinates, and a value with "px" is refused
    // rather than silently read as its numeric prefix.
    size_t a = 0, b = s.size();
    while(a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while(b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    s = s.substr(a, b - a);
    double v = 0.0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if(r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// Splits a run of numbers separated by whitespace and commas.
std::vector<double> numberList(std::string_view s) {
    std::vector<double> out;
    size_t i = 0;
    while(i < s.size()) {
        while(i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\t' ||
                               s[i] == '\r'))
            i++;
        const size_t start = i;
        while(i < s.size() && s[i] != ' ' && s[i] != ',' && s[i] != '\n' && s[i] != '\t' &&
              s[i] != '\r')
            i++;
        if(i > start) {
            if(const std::optional<double> v = number(s.substr(start, i - start))) out.push_back(*v);
        }
    }
    return out;
}

class Tracer {
public:
    SvgImport run(std::string_view svg) {
        scan(svg);
        return {std::move(doc_), traced_, skipped_};
    }

private:
    // doc y is up, svg y is down. Fold the negative zero -0.0 produces at the
    // origin back to 0.0, so a traced document does not serialize "-0" where the
    // exporter wrote "0".
    static double invY(double y) {
        const double r = -y;
        return r == 0.0 ? 0.0 : r;
    }

    EntityId addPoint(double x, double y) {
        EntityRecord r;
        r.kind = EntityKind::Point;
        r.seeds = {x, invY(y)};
        const CommandResult res = doc_.apply(AddRecord<EntityRecord>{r});
        return res.ok() ? EntityId(res.allocated) : EntityId();
    }

    void addSegment(EntityId a, EntityId b) {
        if(!a.valid() || !b.valid() || a == b) return;
        EntityRecord r;
        r.kind = EntityKind::Segment;
        r.points = {a, b, EntityId()};
        doc_.apply(AddRecord<EntityRecord>{r});
    }

    void traceLine(std::string_view a) {
        const auto x1 = attr(a, "x1"), y1 = attr(a, "y1"), x2 = attr(a, "x2"), y2 = attr(a, "y2");
        if(!x1 || !y1 || !x2 || !y2) { skipped_++; return; }
        const auto nx1 = number(*x1), ny1 = number(*y1), nx2 = number(*x2), ny2 = number(*y2);
        if(!nx1 || !ny1 || !nx2 || !ny2) { skipped_++; return; }
        addSegment(addPoint(*nx1, *ny1), addPoint(*nx2, *ny2));
        traced_++;
    }

    void tracePolyline(std::string_view a, bool close) {
        const auto pts = attr(a, "points");
        if(!pts) { skipped_++; return; }
        const std::vector<double> flat = numberList(*pts);
        if(flat.size() < 4) { skipped_++; return; }
        std::vector<EntityId> ids;
        for(size_t i = 0; i + 1 < flat.size(); i += 2) ids.push_back(addPoint(flat[i], flat[i + 1]));
        for(size_t i = 0; i + 1 < ids.size(); i++) addSegment(ids[i], ids[i + 1]);
        if(close && ids.size() > 2) addSegment(ids.back(), ids.front());
        traced_++;
    }

    void traceRect(std::string_view a) {
        const auto x = attr(a, "x"), y = attr(a, "y"), w = attr(a, "width"), h = attr(a, "height");
        if(!w || !h) { skipped_++; return; }
        const double x0 = x ? number(*x).value_or(0.0) : 0.0;
        const double y0 = y ? number(*y).value_or(0.0) : 0.0;
        const auto nw = number(*w), nh = number(*h);
        if(!nw || !nh) { skipped_++; return; }
        const EntityId a0 = addPoint(x0, y0);
        const EntityId a1 = addPoint(x0 + *nw, y0);
        const EntityId a2 = addPoint(x0 + *nw, y0 + *nh);
        const EntityId a3 = addPoint(x0, y0 + *nh);
        addSegment(a0, a1);
        addSegment(a1, a2);
        addSegment(a2, a3);
        addSegment(a3, a0);
        traced_++;
    }

    void traceCircle(std::string_view a) {
        const auto cx = attr(a, "cx"), cy = attr(a, "cy"), rr = attr(a, "r");
        if(!rr) { skipped_++; return; }
        const double x0 = cx ? number(*cx).value_or(0.0) : 0.0;
        const double y0 = cy ? number(*cy).value_or(0.0) : 0.0;
        const auto radius = number(*rr);
        if(!radius || !(*radius > 0.0)) { skipped_++; return; }
        const EntityId centre = addPoint(x0, y0);
        if(!centre.valid()) { skipped_++; return; }
        EntityRecord r;
        r.kind = EntityKind::Circle;
        r.points = {centre, EntityId(), EntityId()};
        r.seeds = {*radius, 0.0};
        doc_.apply(AddRecord<EntityRecord>{r});
        traced_++;
    }

    // Straight-line paths only: M/m, L/l, H/h, V/v, Z/z, with implicit command
    // repetition (a coordinate pair after an L continues the L). The vertices and
    // segments are buffered and committed to the document only if the whole path
    // parses — a curve command aborts the element with nothing added, so "skipped
    // whole" is true rather than "skipped after leaving the moveto point behind".
    void tracePath(std::string_view a) {
        const auto d = attr(a, "d");
        if(!d) {
            skipped_++;
            return;
        }
        const std::string_view s = *d;

        std::vector<Point> verts;
        std::vector<std::pair<size_t, size_t>> segs;
        size_t subStart = static_cast<size_t>(-1);  // current subpath's first vertex
        size_t cur = static_cast<size_t>(-1);        // current vertex
        double curX = 0.0, curY = 0.0;
        size_t i = 0;

        auto readNums = [&](size_t count) -> std::optional<std::vector<double>> {
            std::vector<double> out;
            while(out.size() < count) {
                while(i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\n' || s[i] == '\t' ||
                                       s[i] == '\r'))
                    i++;
                const size_t start = i;
                while(i < s.size() && s[i] != ' ' && s[i] != ',' && s[i] != '\n' && s[i] != '\t' &&
                      s[i] != '\r' && !(std::isalpha(static_cast<unsigned char>(s[i]))))
                    i++;
                if(i == start) return std::nullopt;
                const std::optional<double> v = number(s.substr(start, i - start));
                if(!v) return std::nullopt;
                out.push_back(*v);
            }
            return out;
        };
        auto lineTo = [&](double x, double y) {
            verts.push_back({x, y});
            const size_t idx = verts.size() - 1;
            if(cur != static_cast<size_t>(-1)) segs.push_back({cur, idx});
            cur = idx;
            curX = x;
            curY = y;
        };

        char implicit = 0;  // the command a bare coordinate set repeats
        while(i < s.size()) {
            while(i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ','))
                i++;
            if(i >= s.size()) break;

            char cmd;
            if(std::isalpha(static_cast<unsigned char>(s[i]))) {
                cmd = s[i];
                i++;
            } else if(implicit != 0) {
                cmd = implicit;  // implicit repetition of the previous command
            } else {
                skipped_++;
                return;
            }

            switch(cmd) {
                case 'M': case 'm': case 'L': case 'l': {
                    const bool rel = (cmd == 'm' || cmd == 'l');
                    const bool move = (cmd == 'M' || cmd == 'm');
                    const auto v = readNums(2);
                    if(!v) { skipped_++; return; }
                    const bool haveCur = cur != static_cast<size_t>(-1);
                    const double x = (*v)[0] + (rel && haveCur ? curX : 0.0);
                    const double y = (*v)[1] + (rel && haveCur ? curY : 0.0);
                    if(move) {
                        verts.push_back({x, y});
                        subStart = verts.size() - 1;
                        cur = subStart;
                        curX = x;
                        curY = y;
                        // A moveto's trailing pairs are implicit linetos, per SVG.
                        implicit = rel ? 'l' : 'L';
                    } else {
                        lineTo(x, y);
                        implicit = cmd;
                    }
                    break;
                }
                case 'H': case 'h': {
                    const auto v = readNums(1);
                    if(!v) { skipped_++; return; }
                    lineTo((*v)[0] + (cmd == 'h' ? curX : 0.0), curY);
                    implicit = cmd;
                    break;
                }
                case 'V': case 'v': {
                    const auto v = readNums(1);
                    if(!v) { skipped_++; return; }
                    lineTo(curX, (*v)[0] + (cmd == 'v' ? curY : 0.0));
                    implicit = cmd;
                    break;
                }
                case 'Z': case 'z':
                    if(cur != static_cast<size_t>(-1) && subStart != static_cast<size_t>(-1) &&
                       cur != subStart) {
                        segs.push_back({cur, subStart});
                    }
                    cur = subStart;
                    if(subStart != static_cast<size_t>(-1)) {
                        curX = verts[subStart].x;
                        curY = verts[subStart].y;
                    }
                    implicit = 0;  // a Z ends a subpath; the next command must be explicit
                    break;
                default:
                    // A curve or an unsupported command: nothing is committed, so
                    // the element is skipped whole rather than partially traced.
                    skipped_++;
                    return;
            }
        }

        // Committed only now the whole path is known straight. Only vertices a
        // segment touches become points, so a lone moveto adds nothing.
        std::vector<EntityId> ids(verts.size());
        auto ensure = [&](size_t idx) {
            if(!ids[idx].valid()) ids[idx] = addPoint(verts[idx].x, verts[idx].y);
            return ids[idx];
        };
        for(const auto &[from, to] : segs) addSegment(ensure(from), ensure(to));
        traced_++;
    }

    void dispatch(std::string_view tag, std::string_view attrs) {
        if(tag == "line") traceLine(attrs);
        else if(tag == "polyline") tracePolyline(attrs, false);
        else if(tag == "polygon") tracePolyline(attrs, true);
        else if(tag == "rect") traceRect(attrs);
        else if(tag == "circle") traceCircle(attrs);
        else if(tag == "path") tracePath(attrs);
        else if(tag == "svg" || tag == "g" || tag == "defs" || tag == "title" || tag == "desc" ||
                tag == "metadata" || tag == "mask" || tag == "clipPath")
            ;  // structural, not geometry — carries no shape of its own
        else
            // Any other element is a shape the trace does not read: an ellipse,
            // text, a raster image, a use reference. Counted, not guessed at.
            skipped_++;
    }

    void scan(std::string_view svg) {
        size_t i = 0;
        while(i < svg.size()) {
            const size_t lt = svg.find('<', i);
            if(lt == std::string_view::npos) break;

            // A comment or CDATA is skipped to its real terminator, so a '>'
            // inside one does not end it early and swallow the markup after it.
            if(svg.substr(lt, 4) == "<!--") {
                const size_t end = svg.find("-->", lt + 4);
                i = (end == std::string_view::npos) ? svg.size() : end + 3;
                continue;
            }
            if(svg.substr(lt, 9) == "<![CDATA[") {
                const size_t end = svg.find("]]>", lt + 9);
                i = (end == std::string_view::npos) ? svg.size() : end + 3;
                continue;
            }
            if(lt + 1 < svg.size() &&
               (svg[lt + 1] == '/' || svg[lt + 1] == '?' || svg[lt + 1] == '!')) {
                const size_t gt = svg.find('>', lt);
                if(gt == std::string_view::npos) break;
                i = gt + 1;
                continue;
            }

            // The tag's closing '>', found while stepping over quoted attribute
            // values so a '>' inside a value does not truncate the tag.
            size_t gt = lt + 1;
            char quote = 0;
            while(gt < svg.size()) {
                const char c = svg[gt];
                if(quote != 0) {
                    if(c == quote) quote = 0;
                } else if(c == '"' || c == '\'') {
                    quote = c;
                } else if(c == '>') {
                    break;
                }
                gt++;
            }
            if(gt >= svg.size()) break;

            const std::string_view inner = svg.substr(lt + 1, gt - lt - 1);
            // Tag name is up to the first space; the rest is attributes.
            size_t nameEnd = 0;
            while(nameEnd < inner.size() && inner[nameEnd] != ' ' && inner[nameEnd] != '\n' &&
                  inner[nameEnd] != '\t' && inner[nameEnd] != '\r' && inner[nameEnd] != '/')
                nameEnd++;
            dispatch(inner.substr(0, nameEnd), inner.substr(nameEnd));
            i = gt + 1;
        }
    }

    Document doc_;
    size_t traced_ = 0;
    size_t skipped_ = 0;
};

}  // namespace

SvgImport readSvg(std::string_view svg) {
    Tracer tracer;
    return tracer.run(svg);
}

}  // namespace paroculus
