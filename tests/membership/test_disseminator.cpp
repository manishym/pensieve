#include "membership/disseminator.h"

#include <set>
#include <gtest/gtest.h>

using namespace pensieve;

namespace {

MembershipUpdate make_update(MembershipUpdate::Type type,
                             const std::string& host, uint16_t port,
                             uint64_t inc = 0) {
    return {type, {host, port}, inc};
}

}  // namespace

TEST(Disseminator, EnqueueAndGet) {
    Disseminator d(10);
    d.enqueue(make_update(MembershipUpdate::Type::Join, "10.0.0.1", 5000, 1));
    EXPECT_EQ(d.pending(), 1u);

    auto updates = d.get_updates(10);
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].node.host, "10.0.0.1");
}

TEST(Disseminator, LimitReturned) {
    Disseminator d(10);
    for (int i = 0; i < 5; ++i) {
        d.enqueue(make_update(MembershipUpdate::Type::Join,
                              "10.0.0." + std::to_string(i), 5000));
    }
    EXPECT_EQ(d.pending(), 5u);

    auto updates = d.get_updates(3);
    EXPECT_EQ(updates.size(), 3u);
}

TEST(Disseminator, EvictionAfterMaxTransmissions) {
    Disseminator d(3);
    d.enqueue(make_update(MembershipUpdate::Type::Join, "10.0.0.1", 5000));

    d.get_updates(10);
    EXPECT_EQ(d.pending(), 1u);

    d.get_updates(10);
    EXPECT_EQ(d.pending(), 1u);

    d.get_updates(10);
    EXPECT_EQ(d.pending(), 0u);
}

TEST(Disseminator, LeastTransmittedFirst) {
    Disseminator d(10);
    d.enqueue(make_update(MembershipUpdate::Type::Join, "old", 5000));
    d.get_updates(10);  // old: count=1

    d.enqueue(make_update(MembershipUpdate::Type::Join, "new", 5000));
    // new: count=0, old: count=1

    auto updates = d.get_updates(1);
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].node.host, "new");
}

TEST(Disseminator, DuplicateNodeSuperseded) {
    Disseminator d(10);
    d.enqueue(make_update(MembershipUpdate::Type::Join, "10.0.0.1", 5000, 1));
    d.enqueue(make_update(MembershipUpdate::Type::Suspect, "10.0.0.1", 5000, 2));
    EXPECT_EQ(d.pending(), 1u);

    auto updates = d.get_updates(10);
    ASSERT_EQ(updates.size(), 1u);
    EXPECT_EQ(updates[0].type, MembershipUpdate::Type::Suspect);
    EXPECT_EQ(updates[0].incarnation, 2u);
}

TEST(Disseminator, DuplicateLowerIncarnationIgnored) {
    Disseminator d(10);
    d.enqueue(make_update(MembershipUpdate::Type::Suspect, "10.0.0.1", 5000, 5));
    d.enqueue(make_update(MembershipUpdate::Type::Join, "10.0.0.1", 5000, 3));
    EXPECT_EQ(d.pending(), 1u);

    auto updates = d.get_updates(10);
    EXPECT_EQ(updates[0].type, MembershipUpdate::Type::Suspect);
    EXPECT_EQ(updates[0].incarnation, 5u);
}

TEST(Disseminator, DuplicateSupersededResetsTransmitCount) {
    Disseminator d(3);
    d.enqueue(make_update(MembershipUpdate::Type::Join, "10.0.0.1", 5000, 1));
    d.get_updates(10);
    d.get_updates(10);
    // transmit_count = 2, one more get would evict

    d.enqueue(make_update(MembershipUpdate::Type::Suspect, "10.0.0.1", 5000, 2));
    // Should reset transmit_count to 0

    d.get_updates(10);
    d.get_updates(10);
    EXPECT_EQ(d.pending(), 1u);  // still alive (count=2, max=3)

    d.get_updates(10);
    EXPECT_EQ(d.pending(), 0u);
}

TEST(Disseminator, EmptyQueue) {
    Disseminator d(10);
    auto updates = d.get_updates(5);
    EXPECT_TRUE(updates.empty());
    EXPECT_EQ(d.pending(), 0u);
}
