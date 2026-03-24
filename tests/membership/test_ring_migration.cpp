#include "membership/ring_migration.h"

#include <algorithm>
#include <gtest/gtest.h>

using namespace pensieve;

namespace {

RingStore::Ring make_ring(
    std::initializer_list<std::pair<uint32_t, NodeId>> entries) {
    RingStore::Ring ring;
    for (const auto& [token, node] : entries) {
        ring[token] = node;
    }
    return ring;
}

const NodeId A{"a", 1};
const NodeId B{"b", 1};
const NodeId C{"c", 1};

}  // namespace

TEST(RingMigration, IdenticalRingsProduceNoMigrations) {
    auto ring = make_ring({{100, A}, {200, B}, {300, C}});
    auto result = compute_migrations(ring, ring);
    EXPECT_TRUE(result.empty());
}

TEST(RingMigration, EmptyOldRing) {
    RingStore::Ring empty;
    auto ring = make_ring({{100, A}});
    auto result = compute_migrations(empty, ring);
    EXPECT_TRUE(result.empty());
}

TEST(RingMigration, EmptyNewRing) {
    auto ring = make_ring({{100, A}});
    RingStore::Ring empty;
    auto result = compute_migrations(ring, empty);
    EXPECT_TRUE(result.empty());
}

TEST(RingMigration, AddNodeTakesRangeFromSuccessor) {
    // Old: A at 100, B at 300.
    //   (300,100] -> A (wrap), (100,300] -> B
    // New: add C at 200.
    //   (300,100] -> A, (100,200] -> C, (200,300] -> B
    // Migration: range (100, 200] moves from B -> C
    auto old_ring = make_ring({{100, A}, {300, B}});
    auto new_ring = make_ring({{100, A}, {200, C}, {300, B}});

    auto migs = compute_migrations(old_ring, new_ring);
    ASSERT_EQ(migs.size(), 1u);
    EXPECT_EQ(migs[0].range_start, 100u);
    EXPECT_EQ(migs[0].range_end, 200u);
    EXPECT_EQ(migs[0].from_node, B);
    EXPECT_EQ(migs[0].to_node, C);
}

TEST(RingMigration, RemoveNodeGivesRangeToSuccessor) {
    // Old: A at 100, C at 200, B at 300.
    //   (300,100] -> A, (100,200] -> C, (200,300] -> B
    // New: remove C. A at 100, B at 300.
    //   (300,100] -> A, (100,300] -> B
    // Migration: range (100, 200] moves from C -> B
    auto old_ring = make_ring({{100, A}, {200, C}, {300, B}});
    auto new_ring = make_ring({{100, A}, {300, B}});

    auto migs = compute_migrations(old_ring, new_ring);
    ASSERT_EQ(migs.size(), 1u);
    EXPECT_EQ(migs[0].range_start, 100u);
    EXPECT_EQ(migs[0].range_end, 200u);
    EXPECT_EQ(migs[0].from_node, C);
    EXPECT_EQ(migs[0].to_node, B);
}

TEST(RingMigration, WrapAroundRange) {
    // Old: B at 100.  B owns the entire ring.
    // New: A at 50, B at 100.
    //   (100,50] -> A (wraps), (50,100] -> B
    // Migration: (100, 50] from B -> A (wrapping range).
    auto old_ring = make_ring({{100, B}});
    auto new_ring = make_ring({{50, A}, {100, B}});

    auto migs = compute_migrations(old_ring, new_ring);
    ASSERT_EQ(migs.size(), 1u);
    EXPECT_EQ(migs[0].range_start, 100u);
    EXPECT_EQ(migs[0].range_end, 50u);
    EXPECT_EQ(migs[0].from_node, B);
    EXPECT_EQ(migs[0].to_node, A);
}

TEST(RingMigration, MultipleRangesMigrate) {
    // Old: A at 100, B at 200, C at 300
    //   (300,100] -> A, (100,200] -> B, (200,300] -> C
    // New: swap A and C: C at 100, B at 200, A at 300
    //   (300,100] -> C, (100,200] -> B, (200,300] -> A
    auto old_ring = make_ring({{100, A}, {200, B}, {300, C}});
    auto new_ring = make_ring({{100, C}, {200, B}, {300, A}});

    auto migs = compute_migrations(old_ring, new_ring);
    // (300,100] changed A->C, (200,300] changed C->A
    ASSERT_EQ(migs.size(), 2u);

    auto find_mig = [&](uint32_t end) -> const RangeMigration* {
        for (const auto& m : migs) {
            if (m.range_end == end) return &m;
        }
        return nullptr;
    };

    auto m1 = find_mig(100);
    ASSERT_NE(m1, nullptr);
    EXPECT_EQ(m1->range_start, 300u);
    EXPECT_EQ(m1->from_node, A);
    EXPECT_EQ(m1->to_node, C);

    auto m2 = find_mig(300);
    ASSERT_NE(m2, nullptr);
    EXPECT_EQ(m2->range_start, 200u);
    EXPECT_EQ(m2->from_node, C);
    EXPECT_EQ(m2->to_node, A);
}

TEST(RingMigration, ThreeNodeAddFourthNode) {
    // Old: A@100, B@200, C@300
    //   (300,100] -> A, (100,200] -> B, (200,300] -> C
    // New: A@100, B@200, D@250, C@300
    //   (300,100] -> A, (100,200] -> B, (200,250] -> D, (250,300] -> C
    // Migration: (200, 250] from C -> D
    NodeId D{"d", 1};
    auto old_ring = make_ring({{100, A}, {200, B}, {300, C}});
    auto new_ring = make_ring({{100, A}, {200, B}, {250, D}, {300, C}});

    auto migs = compute_migrations(old_ring, new_ring);
    ASSERT_EQ(migs.size(), 1u);
    EXPECT_EQ(migs[0].range_start, 200u);
    EXPECT_EQ(migs[0].range_end, 250u);
    EXPECT_EQ(migs[0].from_node, C);
    EXPECT_EQ(migs[0].to_node, D);
}

TEST(RingMigration, SingleNodeToEmpty) {
    auto ring = make_ring({{100, A}});
    RingStore::Ring empty;
    auto result = compute_migrations(ring, empty);
    EXPECT_TRUE(result.empty());
}

TEST(RingMigration, CompleteReplacement) {
    // Old: only A. New: only B at same position.
    auto old_ring = make_ring({{100, A}});
    auto new_ring = make_ring({{100, B}});

    auto migs = compute_migrations(old_ring, new_ring);
    // A->B for the single range
    ASSERT_EQ(migs.size(), 1u);
    EXPECT_EQ(migs[0].from_node, A);
    EXPECT_EQ(migs[0].to_node, B);
}
