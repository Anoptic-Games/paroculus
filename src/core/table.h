// One storage shape for every record kind.
//
// Records are kept sorted by ID, which gives ID-ordered iteration for free.
// That order is not a convenience: determinism is a document property, and
// serialization, solver translation and every property test depend on the same
// declaration producing the same sequence on every machine and every run.
//
// Uniformity here is what lets the command set be three shapes rather than
// twenty-four, and what makes each shape's inverse exact.
#pragma once

#include <algorithm>
#include <vector>

#include "core/ids.h"

namespace paroculus {

// Record must expose `IdType` and a member `id`.
// Invariant: records are sorted strictly ascending by id, and no live record
// carries the null id.
template <typename Record>
class RecordTable {
public:
    using IdType = typename Record::IdType;

    // Issues a fresh ID, overwriting whatever the record carried.
    // Returns the issued ID.
    IdType add(Record record) {
        const IdType id = alloc_.allocate();
        record.id = id;
        records_.push_back(std::move(record));
        return id;
    }

    // Inserts at the record's own ID, for undo, copy-with-fresh-ids and load.
    // Returns false if the ID is null or already live, leaving the table
    // untouched — a collision would rebind live references to a new object.
    bool addAt(Record record) {
        if(!record.id.valid()) return false;
        auto it = lower(record.id);
        if(it != records_.end() && it->id == record.id) return false;
        alloc_.reserveAbove(record.id);
        records_.insert(it, std::move(record));
        return true;
    }

    // Returns false if the ID is not live.
    bool remove(IdType id) {
        auto it = locate(id);
        if(it == records_.end()) return false;
        records_.erase(it);
        return true;
    }

    // Replaces a live record wholesale. Whole-record assignment rather than
    // per-field setters is what keeps the inverse trivially exact: the old
    // value is the inverse, with no field-by-field bookkeeping to get wrong.
    // Returns false if the ID is not live.
    bool set(Record record) {
        auto it = locate(record.id);
        if(it == records_.end()) return false;
        *it = std::move(record);
        return true;
    }

    const Record *find(IdType id) const {
        auto it = locate(id);
        return it == records_.end() ? nullptr : &*it;
    }

    bool contains(IdType id) const { return find(id) != nullptr; }
    size_t size() const { return records_.size(); }
    bool empty() const { return records_.empty(); }

    // ID-ordered. The only iteration order any caller should rely on.
    const std::vector<Record> &records() const { return records_; }

    IdAllocator<IdType> &allocator() { return alloc_; }
    const IdAllocator<IdType> &allocator() const { return alloc_; }

    friend bool operator==(const RecordTable &a, const RecordTable &b) {
        return a.records_ == b.records_ && a.alloc_.next() == b.alloc_.next();
    }
    friend bool operator!=(const RecordTable &a, const RecordTable &b) { return !(a == b); }

private:
    static bool byId(const Record &r, IdType k) { return r.id < k; }

    auto lower(IdType id) { return std::lower_bound(records_.begin(), records_.end(), id, byId); }

    typename std::vector<Record>::iterator locate(IdType id) {
        auto it = lower(id);
        return (it != records_.end() && it->id == id) ? it : records_.end();
    }

    typename std::vector<Record>::const_iterator locate(IdType id) const {
        auto it = std::lower_bound(records_.begin(), records_.end(), id, byId);
        return (it != records_.end() && it->id == id) ? it : records_.end();
    }

    std::vector<Record> records_;
    IdAllocator<IdType> alloc_;
};

}  // namespace paroculus
