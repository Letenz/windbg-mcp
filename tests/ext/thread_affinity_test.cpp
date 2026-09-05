// SPDX-License-Identifier: MIT

#include "lifecycle/thread_affinity.h"

#include <gtest/gtest.h>

#include <future>

TEST(ThreadAffinity, RecognizesOnlyInstallingThread) {
    windbgmcp::lifecycle::ThreadAffinity affinity;
    affinity.CaptureCurrent();
    EXPECT_TRUE(affinity.IsCurrent());

    auto other = std::async(std::launch::async, [&] { return affinity.IsCurrent(); });
    EXPECT_FALSE(other.get());

    affinity.Clear();
    EXPECT_FALSE(affinity.IsCurrent());
}
