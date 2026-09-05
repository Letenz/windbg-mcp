// SPDX-License-Identifier: MIT

#include "lifecycle/session_state.h"

#include <gtest/gtest.h>

using windbgmcp::lifecycle::SessionState;
using windbgmcp::lifecycle::SessionStateMachine;

TEST(SessionState, SuccessfulLifecycle) {
    SessionStateMachine state;
    EXPECT_EQ(state.State(), SessionState::Stopped);
    ASSERT_TRUE(state.BeginStart());
    EXPECT_EQ(state.State(), SessionState::Starting);
    ASSERT_TRUE(state.FinishStart(true));
    EXPECT_EQ(state.State(), SessionState::Running);
    ASSERT_TRUE(state.BeginStop());
    EXPECT_EQ(state.State(), SessionState::Stopping);
    ASSERT_TRUE(state.FinishStop());
    EXPECT_EQ(state.State(), SessionState::Stopped);
}

TEST(SessionState, FailedStartReturnsToStopped) {
    SessionStateMachine state;
    ASSERT_TRUE(state.BeginStart());
    ASSERT_TRUE(state.FinishStart(false));
    EXPECT_EQ(state.State(), SessionState::Stopped);
    EXPECT_TRUE(state.BeginStart());
}

TEST(SessionState, RejectsOverlappingTransitions) {
    SessionStateMachine state;
    ASSERT_TRUE(state.BeginStart());
    EXPECT_FALSE(state.BeginStart());
    EXPECT_FALSE(state.BeginStop());
    ASSERT_TRUE(state.FinishStart(true));
    EXPECT_FALSE(state.BeginStart());
    ASSERT_TRUE(state.BeginStop());
    EXPECT_FALSE(state.BeginStart());
    EXPECT_FALSE(state.BeginStop());
}

TEST(SessionState, FailedTeardownCanReturnToRunning) {
    SessionStateMachine state;
    ASSERT_TRUE(state.BeginStart());
    ASSERT_TRUE(state.FinishStart(true));
    ASSERT_TRUE(state.BeginStop());
    ASSERT_TRUE(state.CancelStop());
    EXPECT_EQ(state.State(), SessionState::Running);
}
