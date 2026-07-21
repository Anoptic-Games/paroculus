// The asynchronous solve path: over-budget components solve off the UI thread.
//
// Synchronous-under-budget is the norm and stays the norm — asynchrony has a
// feel cost, the rubber-banding of a pose that lags the cursor, that a small
// predictable system should never pay. This is the degradation for the component
// that exceeds budget anyway: its solve moves to a worker, the UI never blocks,
// the last coherent pose renders throughout, and stale results are discarded by
// generation so a pose never regresses to an older answer.
//
// The mechanism is snapshot-in, generation-out over lock-free rings. A submit
// hands the worker an immutable document snapshot and a component context, both
// self-contained, so the worker touches nothing the UI thread might be mutating.
// A result carries the whole solved context and its generation; the UI drains
// results, keeps the freshest per component, and never sees a partial one because
// a context is filled entirely before it is pushed. No partial solution is ever
// blended: a drained result is applied whole or not at all.
//
// Determinism is preserved because the worker calls the same solve() the
// synchronous path does, on the same inputs, so an async result equals the
// synchronous one within the solver's own tolerance — asynchrony changes when a
// pose arrives, never what it is.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <vector>

#include "solve/solve.h"
#include "solve/spsc.h"

namespace paroculus {

// A component solve carried back from a worker.
struct SolveResult {
    // Which component this is a solve of. Results supersede within a key and are
    // independent across keys, exactly as components are independent systems.
    uint64_t key = 0;
    uint64_t generation = 0;
    // An opaque caller tag echoed back untouched. The session sets it to the
    // document epoch at submit time and drops a drained result whose tag no longer
    // matches, so a solve of a document the user has since edited is discarded
    // rather than applied over the newer one — the cross-key staleness the
    // per-key generation alone cannot order, since an edit can change a
    // component's key.
    uint64_t tag = 0;
    SolveContext context;   // the solved values, whole
    SolveOutcome outcome;
};

// Keeps, per key, the result with the greatest generation that is not older than
// the greatest already applied for that key; updates `applied` to match. A
// result older than what a key has already shown is stale and dropped — a pose
// must never regress to an answer the UI has moved past — and among a batch, only
// the newest per key survives, because the older ones are already superseded.
//
// Pure and deterministic, so the discard policy is testable without threads: the
// timing that produces out-of-order arrivals is the worker pool's, but what to
// keep given a set of arrivals is arithmetic on generations alone.
std::vector<SolveResult> selectFreshest(std::vector<SolveResult> batch,
                                        std::map<uint64_t, uint64_t> &applied);

class SolveScheduler {
public:
    // workers == 0 runs inline: submit solves on the calling thread and queues
    // the result for drain. The generation and discard logic is identical to the
    // threaded path, which is what makes the deterministic tests test the same
    // code the app runs. workers > 0 spawns that many worker threads.
    explicit SolveScheduler(unsigned workers = defaultWorkerCount());
    ~SolveScheduler();

    SolveScheduler(const SolveScheduler &) = delete;
    SolveScheduler &operator=(const SolveScheduler &) = delete;

    // Submits a component solve and returns its generation. `snapshot` must stay
    // immutable for the worker's lifetime, which a shared_ptr<const Document>
    // guarantees: the UI thread takes one copy and shares a read-only view of it.
    // Non-blocking. A request that finds every worker's queue full is dropped and
    // its generation is still returned — the component keeps its last pose and is
    // resubmitted next frame with a newer generation, which the discard policy
    // then prefers.
    uint64_t submit(uint64_t key, std::shared_ptr<const Document> snapshot, SolveContext context,
                    SolveOptions options = {}, uint64_t tag = 0);

    // Freshest non-stale result per key. Non-blocking; empty when nothing is
    // ready. Applying a result is the caller's job, and applying it is atomic —
    // the whole context or none of it.
    std::vector<SolveResult> drain();

    // True when no request is in flight and no result is waiting. The UI uses it
    // to know a burst has settled. The destructor does not consult it — it stops
    // and joins, dropping any queued results, which is harmless at shutdown.
    bool idle() const;

    unsigned workerCount() const { return static_cast<unsigned>(workers_.size()); }

    // A hook the worker calls after solving and before pushing the result,
    // passed the generation. The injected-delay seam the async determinism tests
    // drive: a test gates it to force a stale result to arrive after a newer one
    // and observe it dropped. Null by default and never touched on the app path.
    //
    // Set it once, before the first submit, while the scheduler is quiescent —
    // it is read by worker threads without synchronisation, so changing it while
    // work is in flight is a data race. A test seam, not a live control.
    void setSolveHook(std::function<void(uint64_t generation)> hook);

    static unsigned defaultWorkerCount();

private:
    struct Request {
        uint64_t key = 0;
        uint64_t generation = 0;
        uint64_t tag = 0;
        std::shared_ptr<const Document> snapshot;
        SolveContext context;
        SolveOptions options;
    };

    // One worker owns one request ring and one result ring, so each ring stays
    // strictly single-producer single-consumer: the UI is the only producer of
    // requests and the only consumer of results, the worker the reverse.
    struct Worker {
        SpscRing<Request> requests{256};
        SpscRing<SolveResult> results{256};
        std::atomic<uint32_t> signal{0};  // bumped to wake the worker
        std::atomic<bool> stop{false};
        std::atomic<uint32_t> inFlight{0};  // requests accepted but not yet returned
    };

    void runWorker(Worker &worker);
    void serve(Worker &worker, Request request);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
    std::atomic<uint64_t> nextGeneration_{1};
    unsigned roundRobin_ = 0;
    std::map<uint64_t, uint64_t> applied_;
    std::function<void(uint64_t)> hook_;

    // Inline mode holds results here, since there is no worker ring to hold them.
    std::vector<SolveResult> inlineResults_;
};

}  // namespace paroculus
