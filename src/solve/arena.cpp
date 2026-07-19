#include "solve/arena.h"

#include <mimalloc.h>

#include <utility>

namespace paroculus {

SolveArena::SolveArena() : heap_(mi_heap_new()) {}

SolveArena::~SolveArena() {
    if(heap_ != nullptr) mi_heap_destroy(static_cast<mi_heap_t *>(heap_));
}

SolveArena::SolveArena(SolveArena &&other) noexcept
    : heap_(std::exchange(other.heap_, nullptr)),
      allocated_(std::exchange(other.allocated_, 0)) {}

SolveArena &SolveArena::operator=(SolveArena &&other) noexcept {
    if(this != &other) {
        if(heap_ != nullptr) mi_heap_destroy(static_cast<mi_heap_t *>(heap_));
        heap_ = std::exchange(other.heap_, nullptr);
        allocated_ = std::exchange(other.allocated_, 0);
    }
    return *this;
}

void *SolveArena::allocate(size_t bytes, size_t align) {
    if(bytes == 0 || heap_ == nullptr) return nullptr;
    // Zeroed: the slvs structs are filled field by field and a stray handle
    // from uninitialised memory is a silent, irreproducible wrong answer.
    void *p = mi_heap_zalloc_aligned(static_cast<mi_heap_t *>(heap_), bytes, align);
    if(p != nullptr) allocated_ += bytes;
    return p;
}

}  // namespace paroculus
