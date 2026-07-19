// A heap whose lifetime is one solve.
//
// Translation scratch — the parameter, entity and constraint arrays handed to
// slvs — is allocated here and released wholesale when the arena dies, so a
// solve's memory lifetime is the solve. That matters more than it looks: the
// interactive loop runs a solve per frame during a drag, and per-allocation
// churn at that rate is exactly the kind of cost that turns a latency budget
// into a latency problem.
//
// mimalloc's heap-arena API is already load-bearing below this seam — the
// vendored solver arenas its own temporaries through mi_heap in
// platformbase.cpp — so this reuses the mechanism rather than introducing one.
// mimalloc.h reaches arena.cpp only, so no consumer of this header inherits it.
#pragma once

#include <cstddef>
#include <new>
#include <type_traits>

namespace paroculus {

class SolveArena {
public:
    SolveArena();
    ~SolveArena();

    // Move-only: two owners would double-destroy the heap, and copying an
    // arena would copy nothing anyone wanted.
    SolveArena(const SolveArena &) = delete;
    SolveArena &operator=(const SolveArena &) = delete;
    SolveArena(SolveArena &&other) noexcept;
    SolveArena &operator=(SolveArena &&other) noexcept;

    // bytes: may be 0, which returns nullptr. align: a power of two.
    // Returns uninitialised storage owned by the arena. Never freed
    // individually; the arena releases everything at once.
    void *allocate(size_t bytes, size_t align);

    // Storage for `count` default-initialised T. T must be trivially
    // destructible, since the arena frees without running destructors.
    template <typename T>
    T *array(size_t count) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "the arena frees wholesale and runs no destructors");
        if(count == 0) return nullptr;
        void *p = allocate(count * sizeof(T), alignof(T));
        if(p == nullptr) throw std::bad_alloc();
        // Element by element rather than placement array-new, which the
        // standard permits to prepend a size cookie the arena did not budget
        // for. It does not on this ABI for a trivially-destructible T, but the
        // loop needs no such assumption and costs nothing.
        T *base = static_cast<T *>(p);
        for(size_t i = 0; i < count; i++) new(base + i) T{};
        return base;
    }

    // Total bytes handed out. The bench harness reads this to keep translation
    // overhead honest and separate from solve time.
    size_t bytesAllocated() const { return allocated_; }

private:
    void *heap_ = nullptr;
    size_t allocated_ = 0;
};

}  // namespace paroculus
