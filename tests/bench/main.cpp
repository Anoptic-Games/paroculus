// Solve benchmarks, run on demand rather than in CI.
//
// The interactive loop's budget is a latency budget and the solver is its
// variable term, so the numbers that matter are measured, not assumed. Three of
// them, per component size:
//
//   cold      a fresh context seeded from the document, solved once. What a
//             file open or an undo pays.
//   warm      the same context solved again from its own solved values. What a
//             drag frame pays, and the one that has to fit in the budget —
//             Newton from a near-solution converges in a step or two.
//   translate the share of each solve spent building the Slvs_System rather
//             than solving it. If this ever dominates warm solves, the response
//             is a translation cache keyed by component version, behind the
//             solve seam.
//
// Baselines are per machine and compared as ratios with generous margins;
// absolute ceilings are sanity rails, not targets. Run with --record to write a
// baseline, plain to compare against one.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "solve/solve.h"

using namespace paroculus;

namespace {

const char *BASELINE_PATH = "tests/bench/baseline.txt";

// Provisional budget from the plan: a warm solve of a 256-parameter component
// well inside a frame. A 16ms frame has to hold input, solve, raster and
// present, so a quarter of it is already generous for the solve alone.
constexpr double WARM_BUDGET_US_256 = 4000.0;
// Ratio a measurement may drift above its recorded baseline before the run
// fails. Wide, because these are wall-clock numbers on a shared machine.
constexpr double DRIFT_LIMIT = 3.0;

EntityId addPoint(Document &doc, double x, double y) {
    EntityRecord r;
    r.kind = EntityKind::Point;
    r.seeds = {x, y};
    return EntityId(doc.apply(AddRecord<EntityRecord>{r}).allocated);
}

// A zig-zag chain of segments, alternately held horizontal and equal in length
// to its predecessor, with one end pinned. Chosen because it is connected —
// one component, which is what a solve is scoped to — and because it mixes a
// substitution-handled constraint with one that reaches the Jacobian.
Document buildChain(int points) {
    Document doc;
    std::vector<EntityId> ids;
    ids.reserve(static_cast<size_t>(points));
    for(int i = 0; i < points; i++) {
        ids.push_back(addPoint(doc, i * 10.0, (i % 2 == 0) ? 0.0 : 6.0));
    }

    std::vector<EntityId> segments;
    for(int i = 0; i + 1 < points; i++) {
        EntityRecord s;
        s.kind = EntityKind::Segment;
        s.points = {ids[static_cast<size_t>(i)], ids[static_cast<size_t>(i + 1)], EntityId()};
        segments.push_back(EntityId(doc.apply(AddRecord<EntityRecord>{s}).allocated));
    }

    auto constrain = [&](ConstraintKind kind, std::vector<EntityId> operands, Slot value) {
        ConstraintRecord c;
        c.kind = kind;
        c.value = std::move(value);
        for(size_t i = 0; i < operands.size(); i++) c.operands[i] = operands[i];
        doc.apply(AddRecord<ConstraintRecord>{c});
    };

    constrain(ConstraintKind::Pin, {ids.front()}, Slot());
    for(size_t i = 0; i < segments.size(); i++) {
        if(i % 2 == 0) {
            constrain(ConstraintKind::Horizontal, {segments[i]}, Slot());
        } else if(i > 0) {
            constrain(ConstraintKind::EqualLength, {segments[i - 1], segments[i]}, Slot());
        }
    }
    return doc;
}

struct Sample {
    int params = 0;
    double cold = 0.0;
    double warm = 0.0;
    double translate = 0.0;
    size_t bytes = 0;
};

double median(std::vector<double> &values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

Sample measure(int params) {
    const int points = params / 2;
    const Document doc = buildChain(points);

    Sample sample;
    sample.params = params;

    // Medians rather than means: a single scheduler hiccup should not decide
    // whether a gate passes.
    std::vector<double> cold, warm, translate;
    for(int run = 0; run < 9; run++) {
        SolveContext context = SolveContext::forWholeDocument(doc);
        SolveOptions options;
        options.diagnoseFailures = false;  // the interactive path's setting

        const SolveOutcome first = solve(doc, context, options);
        if(!first.ok()) {
            std::fprintf(stderr, "bench: %d params failed to solve (%s)\n", params,
                         statusName(first.status));
            return sample;
        }
        cold.push_back(first.microseconds);
        translate.push_back(first.translateMicroseconds);
        sample.bytes = first.arenaBytes;

        // Warm: the same context, already at the solution, as a drag frame
        // would find it.
        const SolveOutcome again = solve(doc, context, options);
        warm.push_back(again.microseconds);
    }

    sample.cold = median(cold);
    sample.warm = median(warm);
    sample.translate = median(translate);
    return sample;
}

std::vector<Sample> run() {
    std::vector<Sample> samples;
    // 8..1024 parameters, geometric.
    for(int params = 8; params <= 1024; params *= 2) samples.push_back(measure(params));
    return samples;
}

void write(const std::vector<Sample> &samples, const char *path) {
    std::ofstream out(path);
    out << "# paroculus solve bench baseline\n";
    out << "# params cold_us warm_us translate_us arena_bytes\n";
    for(const Sample &s : samples) {
        out << s.params << ' ' << s.cold << ' ' << s.warm << ' ' << s.translate << ' '
            << s.bytes << '\n';
    }
}

std::vector<Sample> read(const char *path) {
    std::vector<Sample> samples;
    std::ifstream in(path);
    std::string line;
    while(std::getline(in, line)) {
        if(line.empty() || line[0] == '#') continue;
        Sample s;
        if(std::sscanf(line.c_str(), "%d %lf %lf %lf %zu", &s.params, &s.cold, &s.warm,
                       &s.translate, &s.bytes) == 5) {
            samples.push_back(s);
        }
    }
    return samples;
}

}  // namespace

int main(int argc, char *argv[]) {
    bool record = false;
    for(int i = 1; i < argc; i++) {
        if(std::strcmp(argv[i], "--record") == 0) record = true;
    }

    const std::vector<Sample> samples = run();

    std::printf("%8s %10s %10s %10s %10s %8s\n", "params", "cold us", "warm us", "xlate us",
                "xlate %", "bytes");
    for(const Sample &s : samples) {
        std::printf("%8d %10.1f %10.1f %10.1f %9.0f%% %8zu\n", s.params, s.cold, s.warm,
                    s.translate, s.cold > 0.0 ? 100.0 * s.translate / s.cold : 0.0, s.bytes);
    }

    if(record) {
        write(samples, BASELINE_PATH);
        std::printf("\nrecorded baseline to %s\n", BASELINE_PATH);
        return 0;
    }

    int failures = 0;

    // Absolute sanity rail, independent of any baseline.
    for(const Sample &s : samples) {
        if(s.params == 256 && s.warm > WARM_BUDGET_US_256) {
            std::fprintf(stderr,
                         "FAIL: warm solve of a 256-param component took %.1fus, ceiling %.1fus\n",
                         s.warm, WARM_BUDGET_US_256);
            failures++;
        }
    }

    const std::vector<Sample> baseline = read(BASELINE_PATH);
    if(baseline.empty()) {
        std::printf("\nno baseline at %s; run with --record to write one\n", BASELINE_PATH);
        return failures == 0 ? 0 : 1;
    }

    std::printf("\n%8s %12s %12s\n", "params", "warm ratio", "cold ratio");
    for(const Sample &s : samples) {
        const auto it = std::find_if(baseline.begin(), baseline.end(),
                                     [&](const Sample &b) { return b.params == s.params; });
        if(it == baseline.end()) continue;
        const double warmRatio = it->warm > 0.0 ? s.warm / it->warm : 1.0;
        const double coldRatio = it->cold > 0.0 ? s.cold / it->cold : 1.0;
        std::printf("%8d %12.2f %12.2f\n", s.params, warmRatio, coldRatio);
        if(warmRatio > DRIFT_LIMIT) {
            std::fprintf(stderr, "FAIL: warm solve at %d params drifted %.2fx above baseline\n",
                         s.params, warmRatio);
            failures++;
        }
    }

    return failures == 0 ? 0 : 1;
}
