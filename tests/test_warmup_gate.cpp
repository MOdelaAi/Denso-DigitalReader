#include "camera/warmup_gate.h"

#include <catch2/catch_test_macros.hpp>

using denso::ui::PendingStart;
using Ids = std::vector<int64_t>;

TEST_CASE("a single-model camera starts when its model is ready") {
    PendingStart p;
    p.add(7, {"a.onnx"});
    REQUIRE(p.ready("b.onnx") == Ids{});      // unrelated model
    REQUIRE(p.ready("a.onnx") == Ids{7});     // now satisfied
    REQUIRE(p.empty());
    REQUIRE(p.ready("a.onnx") == Ids{});      // already removed
}

TEST_CASE("a multi-model camera waits for all its models") {
    PendingStart p;
    p.add(1, {"a.onnx", "b.onnx"});
    REQUIRE(p.ready("a.onnx") == Ids{});      // still waiting on b
    REQUIRE(p.ready("b.onnx") == Ids{1});     // both ready now
    REQUIRE(p.empty());
}

TEST_CASE("one ready model can satisfy several cameras at once") {
    PendingStart p;
    p.add(1, {"m.onnx"});
    p.add(2, {"m.onnx"});
    p.add(3, {"m.onnx", "n.onnx"});
    REQUIRE(p.ready("m.onnx") == Ids{1, 2});  // 3 still needs n
    REQUIRE_FALSE(p.empty());
    REQUIRE(p.ready("n.onnx") == Ids{3});
    REQUIRE(p.empty());
}

TEST_CASE("drain returns the remaining cameras for fallback") {
    PendingStart p;
    p.add(1, {"a.onnx"});
    p.add(2, {"b.onnx"});
    p.ready("a.onnx");                         // 1 started
    REQUIRE(p.drain() == Ids{2});             // 2 never got its model
    REQUIRE(p.empty());
    REQUIRE(p.drain() == Ids{});
}

TEST_CASE("duplicate ready of an already-satisfied model is a no-op") {
    PendingStart p;
    p.add(1, {"a.onnx"});
    REQUIRE(p.ready("a.onnx") == Ids{1});
    REQUIRE(p.ready("a.onnx") == Ids{});
}
