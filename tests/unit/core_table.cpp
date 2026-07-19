#include <doctest/doctest.h>

#include "core/records.h"
#include "core/table.h"

using paroculus::LayerId;
using paroculus::LayerRecord;
using paroculus::RecordTable;

namespace {

LayerRecord layer(std::string name) {
    LayerRecord r;
    r.name = std::move(name);
    return r;
}

}  // namespace

TEST_CASE("records stay id-ordered whichever order they arrive in") {
    // Determinism is a document property: serialization, solver translation and
    // every property test depend on one declaration producing one sequence.
    RecordTable<LayerRecord> t;
    LayerRecord nine = layer("nine");
    nine.id = LayerId(9);
    LayerRecord three = layer("three");
    three.id = LayerId(3);
    LayerRecord six = layer("six");
    six.id = LayerId(6);

    REQUIRE(t.addAt(nine));
    REQUIRE(t.addAt(three));
    REQUIRE(t.addAt(six));

    REQUIRE(t.records().size() == 3);
    CHECK(t.records()[0].id == LayerId(3));
    CHECK(t.records()[1].id == LayerId(6));
    CHECK(t.records()[2].id == LayerId(9));
}

TEST_CASE("add issues fresh ids and never repeats one") {
    RecordTable<LayerRecord> t;
    const LayerId a = t.add(layer("a"));
    const LayerId b = t.add(layer("b"));
    CHECK(a < b);

    REQUIRE(t.remove(a));
    const LayerId c = t.add(layer("c"));
    // The freed id must not come back.
    CHECK(c != a);
    CHECK(b < c);
}

TEST_CASE("addAt raises the watermark so loaded ids are never reissued") {
    RecordTable<LayerRecord> t;
    LayerRecord high = layer("high");
    high.id = LayerId(100);
    REQUIRE(t.addAt(high));
    CHECK(t.add(layer("next")) == LayerId(101));
}

TEST_CASE("addAt refuses a collision and refuses the null id") {
    RecordTable<LayerRecord> t;
    LayerRecord first = layer("first");
    first.id = LayerId(4);
    REQUIRE(t.addAt(first));

    LayerRecord clash = layer("clash");
    clash.id = LayerId(4);
    CHECK_FALSE(t.addAt(clash));
    CHECK(t.find(LayerId(4))->name == "first");

    CHECK_FALSE(t.addAt(layer("null")));
}

TEST_CASE("set replaces a live record and refuses a dead one") {
    RecordTable<LayerRecord> t;
    const LayerId id = t.add(layer("before"));

    LayerRecord updated = layer("after");
    updated.id = id;
    REQUIRE(t.set(updated));
    CHECK(t.find(id)->name == "after");

    LayerRecord orphan = layer("orphan");
    orphan.id = LayerId(999);
    CHECK_FALSE(t.set(orphan));
}

TEST_CASE("remove reports whether anything was there") {
    RecordTable<LayerRecord> t;
    const LayerId id = t.add(layer("a"));
    CHECK(t.contains(id));
    CHECK(t.remove(id));
    CHECK_FALSE(t.contains(id));
    CHECK_FALSE(t.remove(id));
    CHECK(t.find(id) == nullptr);
    CHECK(t.empty());
}

TEST_CASE("table equality covers records and the watermark") {
    RecordTable<LayerRecord> a, b;
    CHECK(a == b);

    const LayerId ia = a.add(layer("x"));
    const LayerId ib = b.add(layer("x"));
    CHECK(ia == ib);
    CHECK(a == b);

    // Same records, different watermark: not equal, because reloading b would
    // hand out an id a has already spent.
    REQUIRE(a.remove(ia));
    REQUIRE(b.remove(ib));
    a.add(layer("y"));
    CHECK(a != b);
}
