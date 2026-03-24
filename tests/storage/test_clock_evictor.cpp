#include "storage/clock_evictor.h"

#include <cstring>
#include <set>
#include <vector>
#include <gtest/gtest.h>

using namespace pensieve;

namespace {

// Helper: allocate a SlabEntry on the heap for testing the evictor
// in isolation (no real slab backing needed).
SlabEntry* make_entry(uint8_t access = 0) {
    auto* buf = new char[sizeof(SlabEntry) + 8];
    std::memset(buf, 0, sizeof(SlabEntry) + 8);
    auto* e = reinterpret_cast<SlabEntry*>(buf);
    e->key_size = 3;
    e->value_size = 5;
    e->access_bit = access;
    return e;
}

void free_entry(SlabEntry* e) {
    delete[] reinterpret_cast<char*>(e);
}

struct EntryGuard {
    std::vector<SlabEntry*> entries;
    ~EntryGuard() { for (auto* e : entries) free_entry(e); }
    SlabEntry* add(uint8_t access = 0) {
        entries.push_back(make_entry(access));
        return entries.back();
    }
};

}  // namespace

TEST(ClockEvictor, EmptyEvictReturnsNull) {
    ClockEvictor ev;
    EXPECT_EQ(ev.evict_one(), nullptr);
    EXPECT_EQ(ev.tracked(), 0u);
}

TEST(ClockEvictor, SingleEntryEviction) {
    EntryGuard g;
    ClockEvictor ev;

    auto* e = g.add(0);
    ev.track(e);
    EXPECT_EQ(ev.tracked(), 1u);

    SlabEntry* victim = ev.evict_one();
    EXPECT_EQ(victim, e);
    EXPECT_EQ(ev.tracked(), 0u);
}

TEST(ClockEvictor, EvictsUntouchedFirst) {
    EntryGuard g;
    ClockEvictor ev;

    auto* e1 = g.add(0);
    auto* e2 = g.add(0);
    auto* e3 = g.add(0);
    ev.track(e1);
    ev.track(e2);
    ev.track(e3);

    // First eviction should return the first untouched entry.
    SlabEntry* victim = ev.evict_one();
    EXPECT_EQ(victim, e1);
    EXPECT_EQ(ev.tracked(), 2u);
}

TEST(ClockEvictor, SecondChanceSpares) {
    EntryGuard g;
    ClockEvictor ev;

    auto* accessed = g.add(1);  // access_bit = 1 (recently used)
    auto* untouched = g.add(0); // access_bit = 0
    ev.track(accessed);
    ev.track(untouched);

    // The clock should skip `accessed` (clearing its bit) and evict `untouched`.
    SlabEntry* victim = ev.evict_one();
    EXPECT_EQ(victim, untouched);
    EXPECT_EQ(accessed->access_bit, 0);  // bit was cleared
    EXPECT_EQ(ev.tracked(), 1u);
}

TEST(ClockEvictor, AllAccessedRequiresFullSweep) {
    EntryGuard g;
    ClockEvictor ev;

    auto* e1 = g.add(1);
    auto* e2 = g.add(1);
    auto* e3 = g.add(1);
    ev.track(e1);
    ev.track(e2);
    ev.track(e3);

    // All have access_bit=1. First sweep clears all bits, second sweep
    // evicts the first entry encountered.
    SlabEntry* victim = ev.evict_one();
    ASSERT_NE(victim, nullptr);

    // All remaining entries should have had their bits cleared.
    EXPECT_EQ(ev.tracked(), 2u);

    // The other two should have access_bit=0 now.
    std::set<SlabEntry*> remaining;
    for (auto* e : {e1, e2, e3}) {
        if (e != victim) remaining.insert(e);
    }
    for (auto* e : remaining) {
        EXPECT_EQ(e->access_bit, 0);
    }
}

TEST(ClockEvictor, UntrackPreventsEviction) {
    EntryGuard g;
    ClockEvictor ev;

    auto* e1 = g.add(0);
    auto* e2 = g.add(0);
    ev.track(e1);
    ev.track(e2);

    ev.untrack(e1);
    EXPECT_EQ(ev.tracked(), 1u);

    SlabEntry* victim = ev.evict_one();
    EXPECT_EQ(victim, e2);
    EXPECT_EQ(ev.tracked(), 0u);
}

TEST(ClockEvictor, UntrackNonexistentIsNoOp) {
    EntryGuard g;
    ClockEvictor ev;

    auto* e = g.add(0);
    ev.untrack(e);  // should not crash
    EXPECT_EQ(ev.tracked(), 0u);
}

TEST(ClockEvictor, EvictAllOneByOne) {
    EntryGuard g;
    ClockEvictor ev;
    constexpr int N = 10;

    std::set<SlabEntry*> all;
    for (int i = 0; i < N; ++i) {
        auto* e = g.add(0);
        ev.track(e);
        all.insert(e);
    }

    std::set<SlabEntry*> evicted;
    for (int i = 0; i < N; ++i) {
        SlabEntry* v = ev.evict_one();
        ASSERT_NE(v, nullptr);
        EXPECT_TRUE(evicted.insert(v).second) << "Duplicate eviction";
    }

    EXPECT_EQ(evicted, all);
    EXPECT_EQ(ev.tracked(), 0u);
    EXPECT_EQ(ev.evict_one(), nullptr);
}

TEST(ClockEvictor, InterleavedAccessAndEviction) {
    EntryGuard g;
    ClockEvictor ev;

    auto* e1 = g.add(0);
    auto* e2 = g.add(0);
    auto* e3 = g.add(0);
    ev.track(e1);
    ev.track(e2);
    ev.track(e3);

    // Touch e1 so it gets second chance.
    e1->access_bit = 1;

    SlabEntry* v1 = ev.evict_one();
    // e1 is accessed -> skip (clear bit), e2 is untouched -> evict.
    EXPECT_EQ(v1, e2);

    // Touch e1 again.
    e1->access_bit = 1;

    SlabEntry* v2 = ev.evict_one();
    // e1 accessed -> skip, e3 untouched -> evict.
    EXPECT_EQ(v2, e3);

    // Only e1 remains.
    SlabEntry* v3 = ev.evict_one();
    EXPECT_EQ(v3, e1);

    EXPECT_EQ(ev.tracked(), 0u);
}

TEST(ClockEvictor, TrackAfterEviction) {
    EntryGuard g;
    ClockEvictor ev;

    auto* e1 = g.add(0);
    ev.track(e1);
    ev.evict_one();

    // Add new entry after full eviction.
    auto* e2 = g.add(0);
    ev.track(e2);
    EXPECT_EQ(ev.tracked(), 1u);

    SlabEntry* v = ev.evict_one();
    EXPECT_EQ(v, e2);
}
