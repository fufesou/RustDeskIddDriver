#include "Driver.h"

namespace display {
HRESULT ColorOutput::Acquire(IDDCX_SWAPCHAIN chain, AcquiredFrame& frame)
{
#if IDDCX_VERSION_MINOR >= 10
    if (AdvancedColorAvailable()) {
        IDARG_IN_RELEASEANDACQUIREBUFFER2 in = {};
        in.Size = sizeof(in);
        IDARG_OUT_RELEASEANDACQUIREBUFFER2 out = {};
        HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer2(chain, &in, &out);
        if (SUCCEEDED(hr)) {
            frame.Attach(out.MetaData.pSurface, out.MetaData.PresentationFrameNumber);
            Frame(out.MetaData);
        }
        return hr;
    }
#endif
    IDARG_OUT_RELEASEANDACQUIREBUFFER out = {};
    HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(chain, &out);
    if (SUCCEEDED(hr)) {
        frame.Attach(out.MetaData.pSurface, out.MetaData.PresentationFrameNumber);
        std::lock_guard<std::mutex> guard(lock);
        if (!state.FrameAvailable) ++generation;
        state.FrameAvailable = TRUE;
        state.PresentationFrameNumber = out.MetaData.PresentationFrameNumber;
        state.ColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        state.SdrWhiteNits = IDD_SDR_WHITE_NITS;
    }
    return hr;
}

#if IDDCX_VERSION_MINOR >= 10
static NTSTATUS NTAPI QueryInfo(IDDCX_ADAPTER, IDARG_IN_QUERYTARGET_INFO* in, IDARG_OUT_QUERYTARGET_INFO* out)
{
    if (in->ConnectorIndex >= IDD_MAX_CONNECTORS) return STATUS_INVALID_PARAMETER;
    *out = {};
    // Windows queries each connector before arrival. EDID and mode depths describe the attached monitor.
    out->TargetCaps = IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE | IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE;
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI CommitColor(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES2* in)
{
    for (UINT i = 0; i < in->PathCount; ++i) {
        const auto& path = in->pPaths[i];
        GetMonitor(path.MonitorObject).color.Commit(path);
    }
    return STATUS_SUCCESS;
}
#endif

void ConfigureColor(IDD_CX_CLIENT_CONFIG& config)
{
#if IDDCX_VERSION_MINOR >= 10
    if (AdvancedColorAvailable()) {
        config.EvtIddCxAdapterQueryTargetInfo = QueryInfo;
        config.EvtIddCxAdapterCommitModes2 = CommitColor;
        config.EvtIddCxMonitorSetDefaultHdrMetaData = [](IDDCX_MONITOR h, const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* in) {
            return GetMonitor(h).color.SetDefault(*in);
        };
        config.EvtIddCxMonitorSetGammaRamp = [](IDDCX_MONITOR h, const IDARG_IN_SET_GAMMARAMP* in) {
            return GetMonitor(h).color.SetGamma(*in);
        };
    }
#else
    UNREFERENCED_PARAMETER(config);
#endif
}
} // namespace display
