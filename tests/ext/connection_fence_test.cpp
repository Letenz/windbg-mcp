// SPDX-License-Identifier: MIT

#include "ipc/connection_fence.h"

#include <gtest/gtest.h>

using windbgmcp::ipc::ConnectionFence;
using windbgmcp::ipc::kNoConnectionGeneration;

TEST(ConnectionFence, ClosedGenerationIsRejected) {
    ConnectionFence fence;
    const auto first = fence.Open();
    EXPECT_TRUE(fence.Allows(first));

    fence.Close(first);
    EXPECT_EQ(fence.Current(), kNoConnectionGeneration);
    EXPECT_FALSE(fence.Allows(first));
}

TEST(ConnectionFence, ReconnectFencesOldGeneration) {
    ConnectionFence fence;
    const auto first = fence.Open();
    fence.Close(first);
    const auto second = fence.Open();

    EXPECT_NE(first, second);
    EXPECT_FALSE(fence.Allows(first));
    EXPECT_TRUE(fence.Allows(second));
}

TEST(ConnectionFence, LateOldCloseCannotClearNewGeneration) {
    ConnectionFence fence;
    const auto first = fence.Open();
    const auto second = fence.Open();

    fence.Close(first);
    EXPECT_TRUE(fence.Allows(second));
}
