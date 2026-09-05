// SPDX-License-Identifier: MIT

#pragma once

namespace windbgmcp::lifecycle {

enum class SessionState {
    Stopped,
    Starting,
    Running,
    Stopping,
};

inline const char* ToString(SessionState state) noexcept {
    switch (state) {
        case SessionState::Stopped:  return "stopped";
        case SessionState::Starting: return "starting";
        case SessionState::Running:  return "running";
        case SessionState::Stopping: return "stopping";
    }
    return "unknown";
}

// Small deterministic state machine. Its owner provides synchronization so
// resource publication and state transitions occur under the same mutex.
class SessionStateMachine {
public:
    SessionState State() const noexcept { return m_state; }

    bool BeginStart() noexcept {
        if (m_state != SessionState::Stopped) return false;
        m_state = SessionState::Starting;
        return true;
    }

    bool FinishStart(bool succeeded) noexcept {
        if (m_state != SessionState::Starting) return false;
        m_state = succeeded ? SessionState::Running : SessionState::Stopped;
        return true;
    }

    bool BeginStop() noexcept {
        if (m_state == SessionState::Stopped) return true;
        if (m_state != SessionState::Running) return false;
        m_state = SessionState::Stopping;
        return true;
    }

    bool FinishStop() noexcept {
        if (m_state != SessionState::Stopping && m_state != SessionState::Stopped) {
            return false;
        }
        m_state = SessionState::Stopped;
        return true;
    }

    bool CancelStop() noexcept {
        if (m_state != SessionState::Stopping) return false;
        m_state = SessionState::Running;
        return true;
    }

private:
    SessionState m_state = SessionState::Stopped;
};

} // namespace windbgmcp::lifecycle
