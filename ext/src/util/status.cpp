// SPDX-License-Identifier: MIT
#include "util/status.h"

namespace windbgmcp::status {

const char* ToString(ULONG s) {
    switch (s) {
        case DEBUG_STATUS_NO_CHANGE:           return "NO_CHANGE";
        case DEBUG_STATUS_GO:                  return "GO";
        case DEBUG_STATUS_GO_HANDLED:          return "GO_HANDLED";
        case DEBUG_STATUS_GO_NOT_HANDLED:      return "GO_NOT_HANDLED";
        case DEBUG_STATUS_STEP_OVER:           return "STEP_OVER";
        case DEBUG_STATUS_STEP_INTO:           return "STEP_INTO";
        case DEBUG_STATUS_BREAK:               return "BREAK";
        case DEBUG_STATUS_NO_DEBUGGEE:         return "NO_DEBUGGEE";
        case DEBUG_STATUS_STEP_BRANCH:         return "STEP_BRANCH";
        case DEBUG_STATUS_IGNORE_EVENT:        return "IGNORE_EVENT";
        case DEBUG_STATUS_RESTART_REQUESTED:   return "RESTART_REQUESTED";
        case DEBUG_STATUS_REVERSE_GO:          return "REVERSE_GO";
        case DEBUG_STATUS_REVERSE_STEP_BRANCH: return "REVERSE_STEP_BRANCH";
        case DEBUG_STATUS_REVERSE_STEP_OVER:   return "REVERSE_STEP_OVER";
        case DEBUG_STATUS_REVERSE_STEP_INTO:   return "REVERSE_STEP_INTO";
        case DEBUG_STATUS_OUT_OF_SYNC:         return "OUT_OF_SYNC";
        case DEBUG_STATUS_WAIT_INPUT:          return "WAIT_INPUT";
        case DEBUG_STATUS_TIMEOUT:             return "TIMEOUT";
        default:                               return "UNKNOWN";
    }
}

bool IsRunning(ULONG s) {
    switch (s) {
        case DEBUG_STATUS_GO:
        case DEBUG_STATUS_GO_HANDLED:
        case DEBUG_STATUS_GO_NOT_HANDLED:
        case DEBUG_STATUS_STEP_OVER:
        case DEBUG_STATUS_STEP_INTO:
        case DEBUG_STATUS_STEP_BRANCH:
        case DEBUG_STATUS_REVERSE_GO:
        case DEBUG_STATUS_REVERSE_STEP_BRANCH:
        case DEBUG_STATUS_REVERSE_STEP_OVER:
        case DEBUG_STATUS_REVERSE_STEP_INTO:
            return true;
        default:
            return false;
    }
}

bool IsBroken(ULONG s) {
    return s == DEBUG_STATUS_BREAK;
}

} // namespace windbgmcp::status
