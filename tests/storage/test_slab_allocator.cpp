#include "storage/slab_allocator.h"

#include <cstring>
#include <set>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

// ---------------------------------------------------------------------------
// Arena tests
// ---------------------------------------------------------------------------

TEST(Arena, BasicCreation) {
    Arena arena(4096);
    EXPECT_NE(arena.base(), nullptr);
    EXPECT_EQ(arena.capacity(), 4096u);
}

TEST(Arena, LargeAllocation) {
    constexpr size_t kSize = 4 * 1024 * 1024;  // 4 MB
    Arena arena(kSize);
    EXPECT_NE(arena.base(), nullptr);
    EXPECT_EQ(arena.capacity(), kSize);
}

TEST(Arena, MemoryIsReadWritable) {
    Arena arena(4096);
    auto* ptr = static_cast<uint8_t*>(arena.base());
    std::memset(ptr, 0xAB, 4096);
    EXPECT_EQ(ptr[0], 0xAB);
    EXPECT_EQ(ptr[4095], 0xAB);
}

TEST(Arena, ZeroCapacityThrows) {
    EXPECT_THROW(Arena(0), std::invalid_argument);
}

TEST(Arena, HugepageFallback) {
    // Hugepages may or may not be available; either way, arena should succeed.
    Arena arena(2 * 1024 * 1024);
    EXPECT_NE(arena.base(), nullptr);
}

// ---------------------------------------------------------------------------
// SlabAllocator - class selection
// ---------------------------------------------------------------------------

TEST(SlabAllocator, ClassForSize) {
    EXPECT_EQ(SlabAllocator::class_for_size(1), 0u);
    EXPECT_EQ(SlabAllocator::class_for_size(64), 0u);
    EXPECT_EQ(SlabAllocator::class_for_size(65), 1u);
    EXPECT_EQ(SlabAllocator::class_for_size(128), 1u);
    EXPECT_EQ(SlabAllocator::class_for_size(129), 2u);
    EXPECT_EQ(SlabAllocator::class_for_size(256), 2u);
    EXPECT_EQ(SlabAllocator::class_for_size(1024), 4u);
    EXPECT_EQ(SlabAllocator::class_for_size(1 << 20), SlabAllocator::kNumClasses - 1);
}

TEST(SlabAllocator, TooLargeReturnsOutOfRange) {
    EXPECT_EQ(SlabAllocator::class_for_size((1 << 20) + 1),
              SlabAllocator::kNumClasses);
}

TEST(SlabAllocator, SlotSizeForClass) {
    EXPECT_EQ(SlabAllocator::slot_size_for_class(0), 64u);
    EXPECT_EQ(SlabAllocator::slot_size_for_class(1), 128u);
    EXPECT_EQ(SlabAllocator::slot_size_for_class(2), 256u);
    EXPECT_EQ(SlabAllocator::slot_size_for_class(14), 1u << 20);
}

// ---------------------------------------------------------------------------
// SlabAllocator - allocation basics
// ---------------------------------------------------------------------------

TEST(SlabAllocator, AllocAndDealloc) {
    SlabAllocator alloc(4 * 1024 * 1024);

    void* p = alloc.allocate(100);
    ASSERT_NE(p, nullptr);
    std::memset(p, 0xFF, 100);

    alloc.deallocate(p, 100);
}

TEST(SlabAllocator, MultipleAllocsDifferentPointers) {
    SlabAllocator alloc(4 * 1024 * 1024);

    std::set<void*> ptrs;
    for (int i = 0; i < 100; ++i) {
        void* p = alloc.allocate(64);
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(ptrs.insert(p).second) << "Duplicate pointer at iteration " << i;
    }

    for (void* p : ptrs) {
        alloc.deallocate(p, 64);
    }
}

TEST(SlabAllocator, DeallocAndReuse) {
    SlabAllocator alloc(4 * 1024 * 1024);

    void* p1 = alloc.allocate(64);
    ASSERT_NE(p1, nullptr);
    alloc.deallocate(p1, 64);

    void* p2 = alloc.allocate(64);
    EXPECT_EQ(p1, p2);
}

TEST(SlabAllocator, TooLargeReturnNull) {
    SlabAllocator alloc(4 * 1024 * 1024);
    void* p = alloc.allocate((1 << 20) + 1);
    EXPECT_EQ(p, nullptr);
}

TEST(SlabAllocator, ExhaustPages) {
    // 2 MB arena = 2 pages of 1 MB each.
    SlabAllocator alloc(2 * 1024 * 1024);
    EXPECT_EQ(alloc.pages_total(), 2u);

    // Each 1 MB page holds 1MB/64B = 16384 slots of class 0.
    std::vector<void*> ptrs;
    for (size_t i = 0; i < 2 * 16384; ++i) {
        void* p = alloc.allocate(64);
        if (!p) break;
        ptrs.push_back(p);
    }
    EXPECT_EQ(ptrs.size(), 2u * 16384);
    EXPECT_EQ(alloc.pages_used(), 2u);

    // Next allocation should fail (no pages left for class 0).
    EXPECT_EQ(alloc.allocate(64), nullptr);

    for (void* p : ptrs) {
        alloc.deallocate(p, 64);
    }
}

TEST(SlabAllocator, DifferentSizeClasses) {
    SlabAllocator alloc(8 * 1024 * 1024);

    void* small = alloc.allocate(32);
    void* medium = alloc.allocate(500);
    void* large = alloc.allocate(8192);

    ASSERT_NE(small, nullptr);
    ASSERT_NE(medium, nullptr);
    ASSERT_NE(large, nullptr);

    // They should come from different pages (different classes).
    EXPECT_GE(alloc.pages_used(), 3u);

    alloc.deallocate(small, 32);
    alloc.deallocate(medium, 500);
    alloc.deallocate(large, 8192);
}

TEST(SlabAllocator, PageMetrics) {
    SlabAllocator alloc(4 * 1024 * 1024);
    EXPECT_EQ(alloc.capacity(), 4u * 1024 * 1024);
    EXPECT_EQ(alloc.pages_total(), 4u);
    EXPECT_EQ(alloc.pages_used(), 0u);

    alloc.allocate(64);
    EXPECT_EQ(alloc.pages_used(), 1u);
}

TEST(SlabAllocator, ConcurrentAllocDealloc) {
    SlabAllocator alloc(16 * 1024 * 1024);
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 1000;

    auto worker = [&]() {
        std::vector<void*> ptrs;
        ptrs.reserve(kOpsPerThread);
        for (int i = 0; i < kOpsPerThread; ++i) {
            void* p = alloc.allocate(128);
            if (p) ptrs.push_back(p);
        }
        for (void* p : ptrs) {
            alloc.deallocate(p, 128);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }
}
