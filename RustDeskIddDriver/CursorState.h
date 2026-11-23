#pragma once

namespace display {
template<class StartVideo, class EnableCursor>
HRESULT StartMonitorPath(StartVideo start, EnableCursor enable, NTSTATUS& cursorStatus)
{
    const HRESULT video = start();
    if (SUCCEEDED(video)) {
        try {
            cursorStatus = enable();
        } catch (const std::bad_alloc&) {
            cursorStatus = STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    return video;
}

struct CursorUpdate {
    IDARG_OUT_QUERY_HWCURSOR basic{};
    BOOL positionValid = TRUE;
    UINT positionId = 0;
    UINT white = IDD_SDR_WHITE_NITS;
};

class CursorSnapshot {
public:
    void Prepare();
    void Reset() { state = {}; }
    IDARG_IN_QUERY_HWCURSOR Input();
    NTSTATUS Refresh(IDDCX_MONITOR handle);
    NTSTATUS Accept(const CursorUpdate& update);
    NTSTATUS Write(const CtlCursorQuery& query, OutputBuffer& out) const;
private:
    CtlCursorState state{};
    std::vector<BYTE> pixels;
};
} // namespace display
