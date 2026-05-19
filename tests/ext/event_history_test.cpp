// SPDX-License-Identifier: MIT
#include "events/event_history.h"

#include <gtest/gtest.h>

using windbgmcp::events::Event;
using windbgmcp::events::History;
using windbgmcp::events::NowMs;

TEST(EventHistory, PushAndQuery) {
    History h;
    const auto t0 = NowMs();
    h.Push({t0,     "break", {}});
    h.Push({t0 + 1, "bugcheck", {}});

    auto since_all = h.Since(0);
    ASSERT_EQ(since_all.size(), 2u);
    EXPECT_EQ(since_all[0].kind, "break");
    EXPECT_EQ(since_all[1].kind, "bugcheck");

    auto since_recent = h.Since(t0 + 1);
    ASSERT_EQ(since_recent.size(), 1u);
    EXPECT_EQ(since_recent[0].kind, "bugcheck");
}

TEST(EventHistory, CapacityEvictsOldest) {
    History h;
    h.SetCapacity(3);
    const auto t0 = NowMs();
    for (int i = 0; i < 5; ++i) {
        h.Push({t0 + std::uint64_t(i), "break", {}});
    }
    auto all = h.Since(0);
    ASSERT_EQ(all.size(), 3u);
    // The 2 oldest were dropped, so the first surviving event has ts t0+2.
    EXPECT_EQ(all.front().ts_ms, t0 + 2);
    EXPECT_EQ(all.back().ts_ms,  t0 + 4);
}
