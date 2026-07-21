#include "solve/scheduler.h"

#include <algorithm>
#include <thread>

namespace paroculus {

std::vector<SolveResult> selectFreshest(std::vector<SolveResult> batch,
                                        std::map<uint64_t, uint64_t> &applied) {
    // Per key, the index of the newest arrival that is not older than what the
    // key has already shown. A result older than the applied generation is stale
    // and dropped; among a batch, the older ones for a key are superseded.
    std::map<uint64_t, size_t> best;
    for(size_t i = 0; i < batch.size(); i++) {
        const SolveResult &r = batch[i];
        const auto seen = applied.find(r.key);
        if(seen != applied.end() && r.generation < seen->second) continue;  // stale
        const auto b = best.find(r.key);
        if(b == best.end() || r.generation > batch[b->second].generation) best[r.key] = i;
    }

    // Ordered by key, so the surviving set is a deterministic function of the
    // batch rather than of arrival timing.
    std::vector<SolveResult> out;
    out.reserve(best.size());
    for(const auto &[key, index] : best) {
        applied[key] = batch[index].generation;
        out.push_back(std::move(batch[index]));
    }
    return out;
}

unsigned SolveScheduler::defaultWorkerCount() {
    const unsigned hw = std::thread::hardware_concurrency();
    if(hw <= 1) return 1;
    return std::min(4u, hw - 1);
}

SolveScheduler::SolveScheduler(unsigned workers) {
    for(unsigned i = 0; i < workers; i++) workers_.push_back(std::make_unique<Worker>());
    // Sharing is begun only when there are real worker threads, so a synchronous
    // fallback (workers == 0, inline mode) never puts a lock on the solver.
    if(!workers_.empty()) {
        beginSolverSharing();
        for(auto &w : workers_) {
            Worker *p = w.get();
            threads_.emplace_back([this, p] { runWorker(*p); });
        }
    }
}

SolveScheduler::~SolveScheduler() {
    for(auto &w : workers_) {
        w->stop.store(true, std::memory_order_release);
        w->signal.fetch_add(1, std::memory_order_release);
        w->signal.notify_one();
    }
    for(std::thread &t : threads_)
        if(t.joinable()) t.join();
    if(!workers_.empty()) endSolverSharing();
}

void SolveScheduler::setSolveHook(std::function<void(uint64_t)> hook) { hook_ = std::move(hook); }

void SolveScheduler::serve(Worker &worker, Request request) {
    SolveResult result;
    result.key = request.key;
    result.generation = request.generation;
    result.tag = request.tag;
    result.context = std::move(request.context);
    request.options.generation = request.generation;
    result.outcome = solve(*request.snapshot, result.context, request.options);

    // After the solve and before the result is visible: the seam a test gates to
    // hold one generation back while a newer one overtakes it.
    if(hook_) hook_(request.generation);

    // Spin the result into the ring. The UI drains steadily, so a full result
    // ring means the UI is momentarily behind rather than gone; yield until it
    // catches up, or bail on shutdown.
    while(!worker.results.push(std::move(result))) {
        if(worker.stop.load(std::memory_order_acquire)) return;
        std::this_thread::yield();
    }
    worker.inFlight.fetch_sub(1, std::memory_order_release);
}

void SolveScheduler::runWorker(Worker &worker) {
    while(!worker.stop.load(std::memory_order_acquire)) {
        if(std::optional<Request> request = worker.requests.pop()) {
            serve(worker, std::move(*request));
            continue;
        }
        // Park until a submit bumps the signal or a stop is requested. Reading the
        // signal before the recheck is what closes the wakeup race: a submit that
        // lands between the empty check and the wait bumps the value the wait is
        // watching, so the wait returns at once.
        const uint32_t token = worker.signal.load(std::memory_order_acquire);
        if(!worker.requests.empty() || worker.stop.load(std::memory_order_acquire)) continue;
        worker.signal.wait(token, std::memory_order_acquire);
    }
}

uint64_t SolveScheduler::submit(uint64_t key, std::shared_ptr<const Document> snapshot,
                                SolveContext context, SolveOptions options, uint64_t tag) {
    const uint64_t generation = nextGeneration_.fetch_add(1, std::memory_order_relaxed);

    Request request;
    request.key = key;
    request.generation = generation;
    request.tag = tag;
    request.snapshot = std::move(snapshot);
    request.context = std::move(context);
    request.options = options;

    if(workers_.empty()) {
        // Inline: solve now, on the caller's thread, and queue the result. The
        // generation and discard logic downstream is identical to the threaded
        // path, so the deterministic tests test the code the app runs.
        SolveResult result;
        result.key = request.key;
        result.generation = request.generation;
        result.tag = request.tag;
        result.context = std::move(request.context);
        request.options.generation = request.generation;
        result.outcome = solve(*request.snapshot, result.context, request.options);
        if(hook_) hook_(request.generation);
        inlineResults_.push_back(std::move(result));
        return generation;
    }

    Worker &worker = *workers_[roundRobin_ % workers_.size()];
    roundRobin_++;
    worker.inFlight.fetch_add(1, std::memory_order_relaxed);
    if(!worker.requests.push(std::move(request))) {
        // Dropped: the component keeps its last pose and is resubmitted next frame
        // with a newer generation, which the discard policy then prefers. The
        // generation is still consumed so nothing downstream mistakes the newer
        // resubmission for a duplicate.
        worker.inFlight.fetch_sub(1, std::memory_order_relaxed);
        return generation;
    }
    worker.signal.fetch_add(1, std::memory_order_release);
    worker.signal.notify_one();
    return generation;
}

std::vector<SolveResult> SolveScheduler::drain() {
    std::vector<SolveResult> batch = std::move(inlineResults_);
    inlineResults_.clear();
    for(auto &w : workers_) {
        while(std::optional<SolveResult> r = w->results.pop()) batch.push_back(std::move(*r));
    }
    return selectFreshest(std::move(batch), applied_);
}

bool SolveScheduler::idle() const {
    if(!inlineResults_.empty()) return false;
    for(const auto &w : workers_) {
        if(w->inFlight.load(std::memory_order_acquire) != 0) return false;
        if(!w->results.empty()) return false;
        if(!w->requests.empty()) return false;
    }
    return true;
}

}  // namespace paroculus
