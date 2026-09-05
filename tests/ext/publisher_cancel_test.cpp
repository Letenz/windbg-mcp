// SPDX-License-Identifier: MIT

#include "events/publisher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <vector>

TEST(PublisherCancel, WakesLongEventWaitDuringTeardown) {
    auto& publisher = windbgmcp::events::Publisher::Get();
    publisher.ResumeWaiters();

    auto wait = std::async(std::launch::async, [&] {
        return publisher.Wait({"never-arrives"}, 30'000, 0);
    });
    publisher.CancelWaiters();

    ASSERT_EQ(wait.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(wait.get().has_value());
    publisher.ResumeWaiters();
}
