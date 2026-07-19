// Persistent document identity.
//
// IDs are never reused and never positional. Constraints, regions, slots, tags
// and undo records all refer across time, so a recycled ID would silently
// rebind a stale reference to a different object, and a positional one would
// invalidate every reference the moment a table compacted. The domain tag makes
// the distinction type-level: an EntityId cannot be passed where a ConstraintId
// is wanted, which is the cheapest possible guard against the one bug this
// design exists to prevent.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace paroculus {

// Domain: an empty tag type naming what the ID identifies. Two Ids over
// different domains are unrelated types.
// Invariant: value 0 is the null ID and is never issued by an allocator.
template <typename Domain>
class Id {
public:
    using Value = uint32_t;

    constexpr Id() = default;
    constexpr explicit Id(Value v) : v_(v) {}

    constexpr Value value() const { return v_; }
    constexpr bool valid() const { return v_ != 0; }
    constexpr explicit operator bool() const { return valid(); }

    friend constexpr bool operator==(Id a, Id b) { return a.v_ == b.v_; }
    friend constexpr bool operator!=(Id a, Id b) { return a.v_ != b.v_; }
    // Ordering is by issue order, which is what gives serialization and solver
    // translation their stable, machine-independent iteration.
    friend constexpr bool operator<(Id a, Id b) { return a.v_ < b.v_; }
    friend constexpr bool operator>(Id a, Id b) { return b < a; }
    friend constexpr bool operator<=(Id a, Id b) { return !(b < a); }
    friend constexpr bool operator>=(Id a, Id b) { return !(a < b); }

private:
    Value v_ = 0;
};

// Domain tags. Declared, never defined: they exist only to name a type.
struct EntityDomain;
struct ConstraintDomain;
struct RegionDomain;
struct TagDomain;
struct StyleDomain;
struct LayerDomain;
struct GroupDomain;
struct ParameterDomain;

using EntityId = Id<EntityDomain>;
using ConstraintId = Id<ConstraintDomain>;
using RegionId = Id<RegionDomain>;
using TagId = Id<TagDomain>;
using StyleId = Id<StyleDomain>;
using LayerId = Id<LayerDomain>;
using GroupId = Id<GroupDomain>;
using ParameterId = Id<ParameterDomain>;

// Issues IDs in increasing order and never repeats one, including across a
// save/load cycle — `next` is serialized, because a document reopened with a
// reset counter would hand a live ID to a new object and silently rebind every
// reference to the old one.
template <typename IdType>
class IdAllocator {
public:
    using Value = typename IdType::Value;

    IdType allocate() { return IdType(next_++); }

    // Raises the watermark so nothing at or below `id` is ever issued. Used on
    // load, where IDs arrive from the file rather than from here.
    void reserveAbove(IdType id) {
        if(id.value() >= next_) next_ = id.value() + 1;
    }

    // The next value that would be issued. Serialized with the document.
    Value next() const { return next_; }
    void setNext(Value v) { next_ = v < 1 ? 1 : v; }

private:
    Value next_ = 1;
};

}  // namespace paroculus

namespace std {

template <typename Domain>
struct hash<paroculus::Id<Domain>> {
    size_t operator()(paroculus::Id<Domain> id) const noexcept {
        return hash<uint32_t>{}(id.value());
    }
};

}  // namespace std
