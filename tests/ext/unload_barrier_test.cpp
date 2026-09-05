// SPDX-License-Identifier: MIT

#include "lifecycle/unload_barrier.h"

#include <gtest/gtest.h>

using windbgmcp::lifecycle::ActivityGuard;
using windbgmcp::lifecycle::ActivityKind;
using windbgmcp::lifecycle::UnloadBarrier;

TEST(UnloadBarrier, TracksEveryOutstandingActivityKind) {
    auto& barrier = UnloadBarrier::Get();
    ASSERT_TRUE(barrier.Snapshot().Quiescent());

    {
        ActivityGuard router(ActivityKind::RouterWorker);
        ActivityGuard callback(ActivityKind::CallbackActor);
        ActivityGuard teardown(ActivityKind::Teardown);
        const auto active = barrier.Snapshot();
        EXPECT_EQ(active.router_workers, 1u);
        EXPECT_EQ(active.callback_actors, 1u);
        EXPECT_EQ(active.teardowns, 1u);
        EXPECT_FALSE(active.Quiescent());
    }

    EXPECT_TRUE(barrier.Snapshot().Quiescent());
}
