#pragma once

#include <mutex>
#include <vector>

namespace display {
struct OutputBuffer {
    BYTE* data;
    size_t size;
    size_t used = 0;
};

inline bool AdvancedColorAvailable()
{
#if IDDCX_VERSION_MINOR >= 10
    return IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterCommitModes2) &&
           IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorUpdateModes2) &&
           IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorQueryHardwareCursor3) &&
           IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2);
#else
    return false;
#endif
}

struct AcquiredFrame {
    Microsoft::WRL::ComPtr<IDXGIResource> surface;
    UINT presentationFrameNumber{};
    LARGE_INTEGER acquireTime{};

    void Attach(IDXGIResource* buffer, UINT number) {
        QueryPerformanceCounter(&acquireTime);
        surface.Attach(buffer);
        presentationFrameNumber = number;
    }
};

class ColorOutput {
public:
    HRESULT Acquire(IDDCX_SWAPCHAIN chain, AcquiredFrame& frame);
    NTSTATUS Read(OutputBuffer& out, const CtlDisplayTarget* target = nullptr);
    void ResetFrames(LUID renderAdapter = {});
    void Commit(UINT space, UINT bits);

#if IDDCX_VERSION_MINOR >= 10
    void Commit(const IDDCX_PATH2& path);
    void Frame(const IDDCX_METADATA2& metadata);
    NTSTATUS SetDefault(const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA& input);
    NTSTATUS SetGamma(const IDARG_IN_SET_GAMMARAMP& input);
private:
    void FrameMetadata(const IDDCX_METADATA2& frame);
    IDDCX_HDR10_METADATA defaultHdr{};
    bool hasDefault = false;
#endif
private:
    std::mutex lock;
    UINT64 generation = 0;
    LUID renderAdapterLuid{};
    CtlColorState state{};
    std::vector<BYTE> gamma;
};
} // namespace display
