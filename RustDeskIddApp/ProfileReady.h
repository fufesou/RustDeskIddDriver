#pragma once
#include <windows.h>
#include <setupapi.h>

namespace profiles {
constexpr DWORD ProfileReadyTimeoutMs = 25000;
constexpr DWORD ProfilePollMs = 250;

struct RequestResult {
    DWORD error;
    bool opening;
    bool reselect = false;
};

inline bool InterfaceWasRemoved(DWORD error)
{
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_NO_SUCH_DEVICE_INTERFACE || error == ERROR_NO_SUCH_DEVINST ||
           error == ERROR_DEVICE_NOT_CONNECTED || error == ERROR_NO_SUCH_DEVICE || error == ERROR_DEVICE_REMOVED;
}

inline bool WaitingForDevice(RequestResult result)
{
    if (result.reselect || result.error == ERROR_NOT_READY) return true;
    if (!result.opening) return false;
    return result.error == ERROR_NO_MORE_ITEMS || InterfaceWasRemoved(result.error);
}

template<class Attempt, class Clock, class SleepFor>
RequestResult RequestWhenReady(Attempt attempt, Clock clock, SleepFor sleepFor)
{
    const ULONGLONG start = clock();
    for (;;) {
        const auto result = attempt();
        const auto elapsed = clock() - start;
        if (!WaitingForDevice(result) || elapsed >= ProfileReadyTimeoutMs) return result;
        const auto remaining = static_cast<DWORD>(ProfileReadyTimeoutMs - elapsed);
        sleepFor(remaining < ProfilePollMs ? remaining : ProfilePollMs);
    }
}
} // namespace profiles
