// SPDX-License-Identifier: MIT

#pragma once

#include <Windows.h>
#include <atomic>

namespace windbgmcp::lifecycle {

class ThreadAffinity {
public:
    void CaptureCurrent() noexcept { m_owner.store(::GetCurrentThreadId()); }
    void Clear() noexcept { m_owner.store(0); }

    bool IsCurrent() const noexcept {
        const DWORD owner = m_owner.load();
        return owner != 0 && owner == ::GetCurrentThreadId();
    }
    DWORD OwnerThreadId() const noexcept { return m_owner.load(); }

private:
    std::atomic<DWORD> m_owner{0};
};

} // namespace windbgmcp::lifecycle
