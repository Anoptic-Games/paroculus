// A bounded single-producer single-consumer lock-free ring.
//
// One thread pushes, one thread pops, and neither takes a lock — which is what
// lets the async solve path carry work to a worker and results back without a
// mutex on the interaction path, per the concurrency rule. The document's single
// writer produces requests; each worker consumes its own; each worker produces
// results; the UI thread consumes them. Every ring therefore has exactly one
// producer and one consumer, which is the whole precondition this structure
// needs to be correct without a lock.
//
// Capacity is a power of two so the wrap is a mask, and one slot is always left
// empty to tell full from empty without a third counter — usable capacity is
// capacity - 1.
#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

namespace paroculus {

template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity = 256) : mask_(roundUpPow2(capacity) - 1), slots_(mask_ + 1) {}

    // Producer side. Returns false when the ring is full; the caller decides what
    // a dropped item means. A superseded solve request is safe to drop because a
    // newer generation will be submitted; that is the whole reason the consumer
    // discards by generation rather than trusting delivery.
    //
    // A forwarding reference so the value is only consumed when the slot is
    // actually taken: a by-value parameter would move the argument even on a full
    // ring, leaving a retry loop pushing a moved-from husk.
    template <typename U>
    bool push(U &&value) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;
        // acquire pairs with the consumer's release of tail_, so a slot freed by
        // the consumer is visible before it is reused.
        if(next == tail_.load(std::memory_order_acquire)) return false;
        slots_[head] = std::forward<U>(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Nullopt when empty.
    std::optional<T> pop() {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if(tail == head_.load(std::memory_order_acquire)) return std::nullopt;
        T value = std::move(slots_[tail]);
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return value;
    }

    bool empty() const {
        return tail_.load(std::memory_order_acquire) == head_.load(std::memory_order_acquire);
    }

    size_t capacity() const { return mask_; }  // usable slots

private:
    static size_t roundUpPow2(size_t n) {
        size_t p = 2;
        while(p < n + 1) p <<= 1;  // +1 for the always-empty slot
        return p;
    }

    size_t mask_;
    std::vector<T> slots_;
    // On separate cache lines: the producer writes head_ and reads tail_, the
    // consumer the reverse, so sharing a line would trade one true dependency for
    // constant false sharing.
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

}  // namespace paroculus
