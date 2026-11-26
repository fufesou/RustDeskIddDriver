#include "Driver.h"
#include <d3d10.h>
#include <tchar.h>
#include "SwapChain.tmh"

using namespace Microsoft::IndirectDisp;
using namespace Microsoft::WRL;

namespace display {
UINT TextureDimension(D3D_FEATURE_LEVEL level)
{
    if (level >= D3D_FEATURE_LEVEL_11_0) return D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (level >= D3D_FEATURE_LEVEL_10_0) return D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (level == D3D_FEATURE_LEVEL_9_3) return D3D_FL9_3_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    return MinimumTextureDimension;
}

static HRESULT AdapterLimit(IDXGIAdapter* adapter, UINT& limit)
{
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr, 0, D3D11_SDK_VERSION, nullptr, &level, nullptr);
    if (SUCCEEDED(hr)) limit = TextureDimension(level);
    return hr;
}

static HRESULT InitialLimit(LUID preferred, UINT& limit)
{
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    if (preferred.LowPart || preferred.HighPart) {
        ComPtr<IDXGIAdapter1> selected;
        hr = factory->EnumAdapterByLuid(preferred, IID_PPV_ARGS(&selected));
        return FAILED(hr) ? hr : AdapterLimit(selected.Get(), limit);
    }
    // Until IddCx assigns the swap chain, use the shared limit of the usable render adapters.
    return CommonRenderLimit([&](UINT index) {
        ComPtr<IDXGIAdapter1> candidate;
        AdapterMeasurement sample{factory->EnumAdapters1(index, &candidate)};
        if (FAILED(sample.enumeration)) return sample;
        sample.status = AdapterLimit(candidate.Get(), sample.dimension);
        if (FAILED(sample.status) && sample.status != DXGI_ERROR_UNSUPPORTED)
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DRIVER, "Skipping unusable render adapter %u: %!HRESULT!", index, sample.status);
        return sample;
    }, limit);
}

NTSTATUS ProbeRenderLimit(LUID preferred, UINT& limit)
{
    const HRESULT hr = InitialLimit(preferred, limit);
    if (SUCCEEDED(hr)) return STATUS_SUCCESS;
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Cannot determine render adapter texture limit %!HRESULT!", hr);
    if (hr == E_OUTOFMEMORY) return STATUS_INSUFFICIENT_RESOURCES;
    if (hr == DXGI_ERROR_NOT_FOUND) return STATUS_NOT_FOUND;
    return hr == DXGI_ERROR_UNSUPPORTED ? STATUS_NOT_SUPPORTED : STATUS_UNSUCCESSFUL;
}

static NTSTATUS MeasureUnassignedAdapter(LUID& adapter, UINT& limit, RenderLimitProbe probe)
{
    const NTSTATUS status = probe(adapter, limit);
    if (status != STATUS_NOT_FOUND || SameAdapter(adapter, {})) return status;
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Measuring available render adapters after the selected GPU was removed");
    adapter = {};
    return probe(adapter, limit);
}

NTSTATUS Device::InitialLimit(InitialTargets& initial) const
{
    if (!FitsMinimumSurface(*initial.requested.modes))
        return MeasureUnassignedAdapter(initial.adapter, initial.limit, probeLimit);
    initial.limit = MinimumTextureDimension;
    return STATUS_SUCCESS;
}

NTSTATUS Monitor::MeasureTargetLimit(const Modes& modes, LUID preferred, RenderLimitProbe probe)
{
    if (FitsMinimumSurface(modes)) return STATUS_SUCCESS;
    const auto needsProbe = [&] { return !assigned && !renderLimit && publishedLimit == MinimumTextureDimension; };
    {
        std::lock_guard<std::mutex> guard(renderLock);
        if (!needsProbe()) return STATUS_SUCCESS;
    }
    UINT measured{};
    const NTSTATUS status = MeasureUnassignedAdapter(preferred, measured, probe);
    std::lock_guard<std::mutex> guard(renderLock);
    if (!needsProbe()) return STATUS_SUCCESS; // A concurrent assignment supplied the actual limit.
    if (NT_SUCCESS(status)) {
        RememberAdapter(preferred);
        lastAdapter = preferred;
        ChangeLimit(measured);
    }
    return status;
}

NTSTATUS Device::RestoreRenderLimit(InitialTargets& previous) const
{
    const UINT published = previous.limit;
    if (published <= MinimumTextureDimension) return STATUS_SUCCESS;
    const LUID first = SameAdapter(preferredGpu, {}) ? previous.adapter : preferredGpu;
    bool useAvailable = SameAdapter(preferredGpu, {}) || SameAdapter(previous.adapter, {});
    NTSTATUS failure = STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    previous.limit = 0;
    const auto consider = [&](LUID adapter) {
        UINT measured{};
        const NTSTATUS status = probeLimit(adapter, measured);
        if (!NT_SUCCESS(status)) {
            TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "Rollback GPU probe failed %!STATUS!", status);
            if (status == STATUS_NOT_FOUND && !SameAdapter(adapter, {})) useAvailable = true;
            else if (failure == STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED) failure = status;
            return false;
        }
        const UINT limit = (std::min)(published, measured);
        if (limit <= previous.limit || !previous.requested.Fits(limit)) return false;
        previous.adapter = adapter;
        previous.limit = limit;
        return limit == published;
    };
    // A legacy mode update can narrow the active list without lowering its published texture limit.
    if (!SameAdapter(first, {}) && consider(first)) return STATUS_SUCCESS;
    for (auto it = previous.adapters.rbegin(); it != previous.adapters.rend(); ++it)
        if (!SameAdapter(first, *it) && consider(*it)) return STATUS_SUCCESS;
    if (useAvailable) consider({});
    if (!previous.limit && previous.requested.Fits(MinimumTextureDimension))
        previous.limit = MinimumTextureDimension;
    return previous.limit ? STATUS_SUCCESS : failure;
}
} // namespace display

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{

}

HRESULT Direct3DDevice::Init()
{
    // The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
    // created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "%!FUNC! cannot create dxgi factory2 %!HRESULT!",
            hr);
        return hr;
    }

    // Find the specified render adapter
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "%!FUNC! cannot enum adapter by luid %!HRESULT!",
            hr);
        return hr;
    }

    // Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
    hr = D3D11CreateDevice(
        Adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &Device,
        nullptr,
        &DeviceContext);
    if (FAILED(hr))
    {
        TraceEvents(TRACE_LEVEL_ERROR,
            TRACE_DRIVER,
            "%!FUNC! cannot create d3d11 device %!HRESULT!",
            hr);

        // If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
        // system is in a transient state.
        return hr;
    }

    return S_OK;
}

SwapChainProcessor::SwapChainProcessor(const IDARG_IN_SETSWAPCHAIN& args,
    std::shared_ptr<Direct3DDevice> device, display::Monitor& monitor)
    : m_Monitor(monitor), m_hSwapChain(args.hSwapChain), m_Device(std::move(device)),
      m_hAvailableBufferEvent(args.hNextSurfaceAvailable)
{}

HRESULT SwapChainProcessor::Start()
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, TRUE, FALSE, nullptr));
    if (!m_hTerminateEvent.IsValid()) return HRESULT_FROM_WIN32(GetLastError());
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
    return m_hThread.IsValid() ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

SwapChainProcessor::~SwapChainProcessor()
{
    if (m_hThread.IsValid()) {
        SetEvent(m_hTerminateEvent.Get());
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID argument)
{
    static_cast<SwapChainProcessor*>(argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    DWORD task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristics(_T("Distribution"), &task);
    RunCore();
    // Returning the chain lets IddCx assign another GPU. Retire this assignment first.
    m_Monitor.ReleaseRenderAssignment();
    m_Monitor.color.ResetFrames();
    WdfObjectDelete(m_hSwapChain);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

static bool CompleteDiscardedFrame(IDDCX_SWAPCHAIN chain, display::AcquiredFrame& frame)
{
    frame.surface.Reset();
    const HRESULT finished = IddCxSwapChainFinishedProcessingFrame(chain);
    if (FAILED(finished)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Cannot finish processing frame %!HRESULT!", finished);
        return false;
    }
    if (!frame.presentationFrameNumber) {
        TraceEvents(TRACE_LEVEL_RESERVED7, TRACE_DRIVER, "Frame completed without a presentation number");
        return true;
    }

    // Capture runs outside the driver. This swap chain discards each frame without encoding it.
    constexpr UINT unslicedFrameCount = 1;
    IDARG_IN_REPORTFRAMESTATISTICS report{};
    auto& stats = report.FrameStatistics;
    stats.Size = sizeof(stats);
    stats.PresentationFrameNumber = frame.presentationFrameNumber;
    stats.FrameStatus = IDDCX_FRAME_STATUS_DROPPED;
    stats.FrameSliceTotal = unslicedFrameCount;
    stats.FrameAcquireQpcTime = static_cast<UINT64>(frame.acquireTime.QuadPart);
    const NTSTATUS status = IddCxSwapChainReportFrameStatistics(chain, &report);
    if (!NT_SUCCESS(status))
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Cannot report frame statistics %!STATUS!", status);
    return NT_SUCCESS(status);
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> dxgi;
    HRESULT hr = m_Device->Device.As(&dxgi);
    if (SUCCEEDED(hr)) {
        IDARG_IN_SWAPCHAINSETDEVICE input = {};
        input.pDevice = dxgi.Get();
        hr = IddCxSwapChainSetDevice(m_hSwapChain, &input);
    }
    if (SUCCEEDED(hr)) {
        m_Monitor.color.ResetFrames(m_Device->AdapterLuid);
        try {
            m_Monitor.ObserveRenderLevel(m_Device->Device->GetFeatureLevel(), m_Device->AdapterLuid);
        } catch (const std::bad_alloc&) {
            hr = E_OUTOFMEMORY;
        }
    }
    HANDLE events[] = {m_hTerminateEvent.Get(), m_hAvailableBufferEvent};
    while (SUCCEEDED(hr) && WaitForSingleObject(events[0], 0) == WAIT_TIMEOUT) {
        display::AcquiredFrame frame;
        hr = m_Monitor.color.Acquire(m_hSwapChain, frame);
        if (hr == E_PENDING) {
            const DWORD result = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) return;
            hr = result == WAIT_OBJECT_0 + 1 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        } else if (SUCCEEDED(hr) && !CompleteDiscardedFrame(m_hSwapChain, frame)) {
            return;
        }
    }
    if (FAILED(hr)) TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "Swap chain stopped %!HRESULT!", hr);
}
