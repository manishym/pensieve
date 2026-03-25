#include "common/types.h"
#include "storage/local_store.h"
#include "storage/slab_allocator.h"
#include "storage/slab_entry.h"

#include <cstddef>
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

// ---------------------------------------------------------------------------
// Static assertions (compile-time guarantees)
// ---------------------------------------------------------------------------

static_assert(alignof(Shard) == kCacheLineSize,
              "Shard must be cache-line aligned");

static_assert(sizeof(SlabEntry) == 16,
              "SlabEntry header must be exactly 16 bytes");

static_assert(SlabAllocator::kMinSlotSize >= kCacheLineSize,
              "Minimum slot size must be at least one cache line");

// ---------------------------------------------------------------------------
// Runtime alignment tests
// ---------------------------------------------------------------------------

TEST(CacheLine, ShardAlignment) {
    // Verify that std::vector places Shards at cache-line boundaries.
    std::vector<Shard> shards(4);
    for (size_t i = 0; i < shards.size(); ++i) {
        auto addr = reinterpret_cast<uintptr_t>(&shards[i]);
        EXPECT_EQ(addr % kCacheLineSize, 0u)
            << "Shard " << i << " not cache-line aligned";
    }
}

TEST(CacheLine, AdjacentShardsNoFalseSharing) {
    std::vector<Shard> shards(2);
    auto addr0 = reinterpret_cast<uintptr_t>(&shards[0]);
    auto addr1 = reinterpret_cast<uintptr_t>(&shards[1]);

    // Adjacent shards must be separated by at least kCacheLineSize bytes.
    EXPECT_GE(addr1 - addr0, kCacheLineSize);
    // And the distance must be a multiple of the cache line.
    EXPECT_EQ((addr1 - addr0) % kCacheLineSize, 0u);
}

TEST(CacheLine, ShardMutexAtStart) {
    // The mutex should be at the very start of Shard (offset 0) so
    // there's no wasted padding before the hot lock field.
    EXPECT_EQ(offsetof(Shard, mu), 0u);
}

TEST(CacheLine, SlabSlotAlignment) {
    // Allocate a few slots from the slab and verify they are at least
    // cache-line aligned (since min slot size is 64 = kCacheLineSize).
    SlabAllocator alloc(4 * 1024 * 1024);
    for (int i = 0; i < 10; ++i) {
        void* p = alloc.allocate(64);
        ASSERT_NE(p, nullptr);
        auto addr = reinterpret_cast<uintptr_t>(p);
        EXPECT_EQ(addr % kCacheLineSize, 0u)
            << "Slot " << i << " not cache-line aligned";
    }
}

TEST(CacheLine, SlabEntryFitsInCacheLine) {
    // Header (16B) + a short key + short value should fit in one cache line.
    EXPECT_LE(SlabEntry::required_size(8, 32), kCacheLineSize);
}
