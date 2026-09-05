// SPDX-License-Identifier: MIT

#include "events/event_sink.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <chrono>
#include <future>
#include <thread>

namespace {

using namespace std::chrono_literals;

bool WaitForState(windbgmcp::events::EventSink* sink,
                  windbgmcp::events::EventSink::InstallState state,
                  std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (sink->State() == state) return true;
        std::this_thread::sleep_for(5ms);
    }
    return sink->State() == state;
}

} // namespace

TEST(EventSinkLifecycle, InstallDoesNotWaitForBlockedDbgEngInitialization) {
    std::promise<void> factory_entered;
    auto factory_entered_future = factory_entered.get_future();
    std::promise<void> release_factory;
    auto release_factory_future = release_factory.get_future().share();
    std::promise<std::thread::id> install_thread;
    auto install_thread_future = install_thread.get_future();
    std::promise<std::thread::id> factory_thread;
    auto factory_thread_future = factory_thread.get_future();

    auto* sink = windbgmcp::events::EventSink::Create(
        [&](IDebugClient** client) -> HRESULT {
            *client = nullptr;
            factory_thread.set_value(std::this_thread::get_id());
            factory_entered.set_value();
            release_factory_future.wait();
            return E_FAIL;
        });

    auto install = std::async(std::launch::async, [&] {
        install_thread.set_value(std::this_thread::get_id());
        return sink->InstallAsync();
    });

    // This is the startup invariant that prevents the live WinDbg deadlock:
    // spawning the actor must not wait for its dbgeng call to complete.
    ASSERT_EQ(install.wait_for(500ms), std::future_status::ready);
    ASSERT_TRUE(install.get());
    ASSERT_EQ(factory_entered_future.wait_for(2s), std::future_status::ready);
    EXPECT_NE(factory_thread_future.get(), install_thread_future.get());
    EXPECT_EQ(sink->State(),
              windbgmcp::events::EventSink::InstallState::Installing);

    release_factory.set_value();
    EXPECT_TRUE(WaitForState(
        sink, windbgmcp::events::EventSink::InstallState::Failed, 2s));
    EXPECT_TRUE(sink->Uninstall());
    EXPECT_EQ(sink->State(),
              windbgmcp::events::EventSink::InstallState::Stopped);
    sink->Release();
}
