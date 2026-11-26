#include "Driver.h"
#include "HardwareCursor.tmh"

namespace display {
Monitor::~Monitor()
{
    // A binding worker can finish while its monitor is being removed.
    {
        std::lock_guard<std::mutex> path(pathLock);
        std::lock_guard<std::mutex> render(renderLock);
        withdrawing = true;
    }
    processor.reset();
}

NTSTATUS Monitor::EnableCursor()
{
    if (!profile->Cursor()) return STATUS_NOT_SUPPORTED;
    if (!cursorEvent.IsValid()) cursorEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    if (!cursorEvent.IsValid()) return STATUS_INSUFFICIENT_RESOURCES;
    cursor.Prepare();
    IDARG_IN_SETUP_HWCURSOR input = {};
    auto& caps = input.CursorInfo;
    caps.Size = sizeof(caps);
    caps.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
    caps.AlphaCursorSupport = TRUE;
    caps.MaxX = caps.MaxY = IDD_CURSOR_EXTENT;
    input.hNewCursorDataAvailable = cursorEvent.Get();
    return IddCxMonitorSetupHardwareCursor(handle, &input);
}

NTSTATUS Monitor::Assign(const IDARG_IN_SETSWAPCHAIN& args)
{
    std::lock_guard<std::mutex> guard(pathLock);
    if (withdrawing) return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    processor.reset();
    ReleaseRenderAssignment();
    BeginRenderAssignment();
    color.ResetFrames();
    cursorValid = false;
    cursor.Reset();
    cursorStatus = STATUS_DEVICE_NOT_READY;
    auto device = std::make_shared<Microsoft::IndirectDisp::Direct3DDevice>(args.RenderAdapterLuid);
    HRESULT hr = device->Init();
    if (FAILED(hr)) return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    auto worker = std::make_unique<SwapChainProcessor>(args, std::move(device), *this);
    hr = StartMonitorPath([&] { return worker->Start(); }, [this] { return EnableCursor(); }, cursorStatus);
    if (FAILED(hr)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Starting swap chain worker failed %!HRESULT!", hr);
        return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
    }
    if (profile->Cursor() && !NT_SUCCESS(cursorStatus))
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Connector %u hardware cursor setup failed %!STATUS!",
                    profile->header.ConnectorIndex, cursorStatus);
    processor = std::move(worker);
    return STATUS_SUCCESS;
}

void Monitor::Unassign()
{
    std::lock_guard<std::mutex> guard(pathLock);
    processor.reset();
    ReleaseRenderAssignment();
    color.ResetFrames();
    cursorValid = false;
}

template<class T> static void CopyCursorUpdate(const T& source, CursorUpdate& result)
{
    result.basic.IsCursorVisible = source.IsCursorVisible;
    result.basic.X = source.X;
    result.basic.Y = source.Y;
    result.basic.IsCursorShapeUpdated = source.IsCursorShapeUpdated;
    result.basic.CursorShapeInfo = source.CursorShapeInfo;
    result.positionValid = source.PositionValid;
    result.positionId = source.PositionId;
}

#if IDDCX_VERSION_MINOR >= 10
static NTSTATUS CursorQueryError(HRESULT hr)
{
    if (hr & FACILITY_NT_BIT) return static_cast<NTSTATUS>(hr & ~FACILITY_NT_BIT);
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32) return NTSTATUS_FROM_WIN32(HRESULT_CODE(hr));
    switch (hr) {
    case E_NOTIMPL: return STATUS_NOT_IMPLEMENTED;
    case E_NOINTERFACE: return STATUS_NOINTERFACE;
    case E_POINTER: return STATUS_INVALID_PARAMETER;
    case E_ABORT: return STATUS_CANCELLED;
    case E_PENDING: return NTSTATUS_FROM_WIN32(ERROR_IO_INCOMPLETE);
    default: return STATUS_UNSUCCESSFUL;
    }
}
#endif

static NTSTATUS QueryCursor(IDDCX_MONITOR handle, const IDARG_IN_QUERY_HWCURSOR& input, CursorUpdate& result)
{
#if IDDCX_VERSION_MINOR >= 10
    if (AdvancedColorAvailable()) {
        IDARG_OUT_QUERY_HWCURSOR3 state{};
        const HRESULT hr = IddCxMonitorQueryHardwareCursor3(handle, &input, &state);
        if (FAILED(hr)) {
            const NTSTATUS status = CursorQueryError(hr);
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Cursor query failed %!HRESULT!, returning %!STATUS!", hr, status);
            return status;
        }
        CopyCursorUpdate(state, result);
        result.white = state.SdrWhiteLevel;
        return STATUS_SUCCESS;
    }
#endif
#if IDDCX_VERSION_MINOR >= 7
    if (IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor2)) {
        IDARG_OUT_QUERY_HWCURSOR2 state{};
        const NTSTATUS status = IddCxMonitorQueryHardwareCursor2(handle, &input, &state);
        if (NT_SUCCESS(status)) CopyCursorUpdate(state, result);
        return status;
    }
#endif
    return IddCxMonitorQueryHardwareCursor(handle, &input, &result.basic);
}

NTSTATUS CursorSnapshot::Refresh(IDDCX_MONITOR handle)
{
    CursorUpdate update;
    const auto in = Input();
    NTSTATUS status = QueryCursor(handle, in, update);
    if (NT_SUCCESS(status)) status = Accept(update);
    if (!NT_SUCCESS(status)) {
        Reset();
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Cursor update failed %!STATUS!", status);
    }
    return status;
}

NTSTATUS Monitor::ReadCursor(const CtlCursorQuery& query, OutputBuffer& out)
{
    std::lock_guard<std::mutex> guard(pathLock);
    if (!profile->Cursor()) return STATUS_NOT_SUPPORTED;
    if (!processor || !AssignedLimit()) return STATUS_DEVICE_NOT_READY;
    if (!NT_SUCCESS(cursorStatus)) return cursorStatus;
    const DWORD wait = WaitForSingleObject(cursorEvent.Get(), 0);
    if (wait == WAIT_FAILED) {
        const DWORD error = GetLastError();
        return NTSTATUS_FROM_WIN32(error);
    }
    if (!cursorValid || wait == WAIT_OBJECT_0) {
        const NTSTATUS status = cursor.Refresh(handle);
        cursorValid = NT_SUCCESS(status);
        if (!cursorValid) return status;
    }
    return cursor.Write(query, out);
}

_Use_decl_annotations_
NTSTATUS AssignSwapChain(IDDCX_MONITOR h, const IDARG_IN_SETSWAPCHAIN* in)
{
    NTSTATUS status;
    try {
        status = GetMonitor(h).Assign(*in);
    } catch (const std::bad_alloc&) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status)) return status;
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Assigning swap chain failed %!STATUS!", status);
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
}

_Use_decl_annotations_
NTSTATUS UnassignSwapChain(IDDCX_MONITOR h)
{
    GetMonitor(h).Unassign();
    return STATUS_SUCCESS;
}
} // namespace display
