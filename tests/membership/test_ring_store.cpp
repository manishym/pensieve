#include "membership/ring_store.h"

#include <thread>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

TEST(RingStore, EmptyByDefault) {
    RingStore rs;
    auto snap = rs.snapshot();
    EXPECT_TRUE(snap->empty());
    EXPECT_EQ(rs.size(), 0u);
}

TEST(RingStore, AddNode) {
    RingStore rs;
    NodeId node{"10.0.0.1", 5000};
    std::vector<uint32_t> tokens{100, 200, 300};
    rs.add_node(node, tokens);

    auto snap = rs.snapshot();
    EXPECT_EQ(snap->size(), 3u);
    EXPECT_EQ(snap->at(100), node);
    EXPECT_EQ(snap->at(200), node);
    EXPECT_EQ(snap->at(300), node);
}

TEST(RingStore, AddMultipleNodes) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    rs.add_node(a, std::vector<uint32_t>{100, 200});
    rs.add_node(b, std::vector<uint32_t>{150, 250});

    auto snap = rs.snapshot();
    EXPECT_EQ(snap->size(), 4u);
    EXPECT_EQ(snap->at(100), a);
    EXPECT_EQ(snap->at(150), b);
    EXPECT_EQ(snap->at(200), a);
    EXPECT_EQ(snap->at(250), b);
}

TEST(RingStore, RemoveNode) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    NodeId b{"10.0.0.2", 5000};

    rs.add_node(a, std::vector<uint32_t>{100, 200});
    rs.add_node(b, std::vector<uint32_t>{150, 250});
    rs.remove_node(a);

    auto snap = rs.snapshot();
    EXPECT_EQ(snap->size(), 2u);
    EXPECT_EQ(snap->count(100), 0u);
    EXPECT_EQ(snap->count(200), 0u);
    EXPECT_EQ(snap->at(150), b);
    EXPECT_EQ(snap->at(250), b);
}

TEST(RingStore, RemoveNonExistent) {
    RingStore rs;
    rs.add_node({"10.0.0.1", 5000}, std::vector<uint32_t>{100});
    rs.remove_node({"10.0.0.99", 5000});
    EXPECT_EQ(rs.size(), 1u);
}

TEST(RingStore, SnapshotIsolation) {
    RingStore rs;
    NodeId a{"10.0.0.1", 5000};
    rs.add_node(a, std::vector<uint32_t>{100, 200});

    auto old_snap = rs.snapshot();
    EXPECT_EQ(old_snap->size(), 2u);

    rs.add_node({"10.0.0.2", 5000}, std::vector<uint32_t>{150});

    // Old snapshot unchanged
    EXPECT_EQ(old_snap->size(), 2u);

    // New snapshot has the addition
    auto new_snap = rs.snapshot();
    EXPECT_EQ(new_snap->size(), 3u);
}

TEST(RingStore, ConcurrentReadWhileWrite) {
    RingStore rs;
    constexpr int kWriters = 4;
    constexpr int kTokensPerWriter = 50;
    constexpr int kReaders = 4;
    constexpr int kReadsPerReader = 200;

    std::vector<std::thread> threads;

    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&rs, w] {
            NodeId node{"10.0.0." + std::to_string(w + 1), 5000};
            for (int t = 0; t < kTokensPerWriter; ++t) {
                uint32_t token = static_cast<uint32_t>(w * 1000 + t);
                rs.add_node(node, std::span<const uint32_t>(&token, 1));
            }
        });
    }

    for (int r = 0; r < kReaders; ++r) {
        threads.emplace_back([&rs] {
            for (int i = 0; i < kReadsPerReader; ++i) {
                auto snap = rs.snapshot();
                // Snapshot must be a consistent view -- iterating shouldn't crash
                size_t count = 0;
                for ([[maybe_unused]] const auto& [token, node] : *snap) {
                    ++count;
                }
                EXPECT_EQ(count, snap->size());
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(rs.size(),
              static_cast<size_t>(kWriters * kTokensPerWriter));
}
