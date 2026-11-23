#pragma once

#define NOMINMAX
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <atomic>
#include <vector>

#include "Trace.h"
#include "Public.h"

namespace Microsoft
{
    namespace WRL
    {
        namespace Wrappers
        {
            // Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
            typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
        }
    }
}

#include "MonitorProfile.h"
#include "AdvancedColor.h"
#include "CursorState.h"

namespace display { class Monitor; }

namespace Microsoft { namespace IndirectDisp {
        struct Direct3DDevice
        {
            Direct3DDevice(LUID AdapterLuid);
            HRESULT Init();

            LUID AdapterLuid;
            Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
            Microsoft::WRL::ComPtr<ID3D11Device> Device;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
        };

        /// <summary>
        /// Manages a thread that consumes buffers from an indirect display swap-chain object.
        /// </summary>
        class SwapChainProcessor
        {
        public:
            SwapChainProcessor(const IDARG_IN_SETSWAPCHAIN& args, std::shared_ptr<Direct3DDevice> device, display::Monitor& monitor);
            HRESULT Start();
            ~SwapChainProcessor();

        private:
            static DWORD CALLBACK RunThread(LPVOID Argument);

            void Run();
            void RunCore();

            display::Monitor& m_Monitor;
            IDDCX_SWAPCHAIN m_hSwapChain;
            std::shared_ptr<Direct3DDevice> m_Device;
            HANDLE m_hAvailableBufferEvent;
            Microsoft::WRL::Wrappers::Thread m_hThread;
            Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
        };
}}

namespace display {
using Modes = std::vector<CtlDisplayMode>;
using Microsoft::IndirectDisp::SwapChainProcessor;
using RenderLimitProbe = NTSTATUS (*)(LUID, UINT&);
NTSTATUS ProbeRenderLimit(LUID preferred, UINT& limit);
UINT TextureDimension(D3D_FEATURE_LEVEL level);
constexpr UINT MinimumTextureDimension = D3D_FL9_1_REQ_TEXTURE2D_U_OR_V_DIMENSION;
inline bool FitsSurface(const CtlDisplayMode& mode, UINT dimension)
{
    return mode.Width <= dimension && mode.Height <= dimension;
}
inline bool HasRenderTarget(const Modes& modes, UINT dimension)
{
    return std::any_of(modes.begin(), modes.end(), [dimension](const auto& mode) { return FitsSurface(mode, dimension); });
}
inline bool FitsMinimumSurface(const Modes& modes)
{
    return std::all_of(modes.begin(), modes.end(), [](const auto& mode) { return FitsSurface(mode, MinimumTextureDimension); });
}
inline bool SameAdapter(LUID first, LUID second)
{
    return first.LowPart == second.LowPart && first.HighPart == second.HighPart;
}

struct InputBuffer {
    const void* data;
    size_t bytes;
};

struct RenderChange {
    UINT limit;
    UINT64 revision;
};

struct TargetModeSnapshot {
    std::shared_ptr<const Modes> candidates, accepted;
    UINT limit;

    const Modes& SourceModes() const {
        // A GPU switch can invalidate the pending update while IddCx is querying it.
        if (candidates != accepted && !HasRenderTarget(*candidates, limit)) return *accepted;
        return *candidates;
    }
};

struct InitialTargets {
    UINT limit;
    std::shared_ptr<const Modes> requested;
    LUID adapter{};
    std::vector<LUID> adapters;
};

struct ReplacementTargets {
    InitialTargets previous, next;
};

class Monitor {
public:
    Monitor(ProfilePtr p, InitialTargets initial, WDFWORKITEM modeWork);
    ~Monitor();
    NTSTATUS Assign(const IDARG_IN_SETSWAPCHAIN& args);
    void Unassign();
    NTSTATUS ReadCursor(const CtlCursorQuery& query, OutputBuffer& out);
    NTSTATUS PublishTargets(const Modes& modes);
    NTSTATUS RefreshTargets(RenderChange change);
    bool ObserveRenderLevel(D3D_FEATURE_LEVEL level, LUID adapter = {});
    void ReleaseRenderAssignment();
    void RetainRenderPreference(LUID previous) {
        std::lock_guard<std::mutex> guard(renderLock);
        RememberAdapter(previous);
    }
    bool PrepareRenderLimit(UINT limit);
    bool PendingRenderChange(RenderChange& change) const;
    bool BeginWithdrawal(RenderChange change);
    void CancelWithdrawal();
    TargetModeSnapshot QueryTargets() const {
        std::lock_guard<std::mutex> guard(renderLock);
        return {targets, requested, renderLimit ? renderLimit : publishedLimit};
    }
    UINT AssignedLimit() const;
    ReplacementTargets PrepareReplacement(std::shared_ptr<const Modes> modes, LUID preferred) const;
    std::shared_ptr<const Modes> RequestedTargets() const { return requested; }

    const ProfilePtr profile;
    IDDCX_MONITOR handle{};
    ColorOutput color;
    std::shared_ptr<const Modes> Targets() const {
        std::lock_guard<std::mutex> guard(renderLock);
        return targets;
    }
private:
    void SetQueryTargets(const std::shared_ptr<const Modes>& modes) {
        std::lock_guard<std::mutex> guard(renderLock);
        targets = modes;
    }
    void BeginRenderAssignment();
    NTSTATUS EnableCursor();
    NTSTATUS SetTargets(std::shared_ptr<const Modes> next, const RenderChange* pending = nullptr);
    NTSTATUS CommitTargets(std::shared_ptr<const Modes> next, RenderChange change);
    void RememberAdapter(LUID adapter);
    bool ChangeLimit(UINT limit);
    const WDFWORKITEM modeWork;
    mutable std::mutex renderLock;
    UINT renderLimit;
    bool assigned = false;
    UINT lastAdapterLimit = 0;
    LUID lastAdapter{};
    std::vector<LUID> adapters;
    UINT pendingPreference = 0;
    UINT64 renderRevision = 0;
    UINT publishedLimit;
    bool republishTargets = false;
    std::shared_ptr<const Modes> requested;
    CursorSnapshot cursor;
    bool cursorValid = false;
    NTSTATUS cursorStatus = STATUS_DEVICE_NOT_READY;
    std::shared_ptr<const Modes> targets;
    std::mutex pathLock;
    bool withdrawing = false;
    Microsoft::WRL::Wrappers::Event cursorEvent;
    std::unique_ptr<SwapChainProcessor> processor;
};

struct MonitorContext { Monitor* instance; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext);
inline Monitor& GetMonitor(IDDCX_MONITOR handle) { return *GetMonitorContext(handle)->instance; }

struct AdapterContext {
    LONG initFinished;
    LONG initStatus;
    PVOID parentDevice;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, GetAdapterContext);

class Device {
public:
    explicit Device(WDFDEVICE device, RenderLimitProbe probe = ProbeRenderLimit) : device(device), probeLimit(probe) {}
    ~Device();
    NTSTATUS InitAdapter();
    void ReleaseAdapter();
    void RefreshRenderModes();
    NTSTATUS Control(ULONG code, InputBuffer input, OutputBuffer& output);

private:
    struct Slot {
        IDDCX_MONITOR handle{};
        ProfilePtr profile;
        GUID attachment{};
        CtlDisplayTarget target{};
    };
    NTSTATUS Attach(ProfilePtr p);
    NTSTATUS InitialLimit(InitialTargets& initial) const;
    NTSTATUS RestoreRenderLimit(InitialTargets& previous) const;
    NTSTATUS Arrive(const ProfilePtr& profile, const GUID& attachment, InitialTargets initial);
    NTSTATUS Detach(UINT index);
    NTSTATUS Replace(UINT index, ProfilePtr profile);
    NTSTATUS Apply(ProfilePtr profile);
    NTSTATUS ApplyRequest(InputBuffer input);
    NTSTATUS DisconnectProfile(InputBuffer input);
    NTSTATUS SessionRequest(ULONG code, InputBuffer input);
    NTSTATUS MatchMonitor(const CtlMonitorIdentity& monitor) const;
    NTSTATUS ReadConnector(InputBuffer input, OutputBuffer& output) const;
    NTSTATUS ReadOutput(ULONG code, InputBuffer input, OutputBuffer& output);
    NTSTATUS LegacyControl(ULONG code, InputBuffer input);
    NTSTATUS Update(const CtlMonitorModes& request, size_t bytes);
    NTSTATUS UpdateProfileModes(InputBuffer input);
    NTSTATUS ChangeModes(UINT index, Modes modes, ModeRequest kind);
    NTSTATUS SetGpu(LUID luid);
    NTSTATUS RefreshRenderLimit(UINT index);
    const WDFDEVICE device;
    const RenderLimitProbe probeLimit;
    LUID preferredGpu{};
    IDDCX_ADAPTER adapter{};
    Slot slots[IDD_MAX_CONNECTORS];
    UINT64 arrivals[IDD_MAX_CONNECTORS]{}; // Survives slot resets while this device handle is open.
    std::mutex controls;
};

struct DeviceContext {
    Device* instance;
    WDFWORKITEM modeWork;
    WDFTIMER modeRetry;
    SRWLOCK restartLock;
    IDDCX_ADAPTER restartAdapter;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);
struct DriverContext { DescriptorDirectory* directory; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DriverContext, GetDriverContext);
inline DescriptorDirectory& Descriptors() { return *GetDriverContext(WdfGetDriver())->directory; }

void ConfigureModes(IDD_CX_CLIENT_CONFIG& config);
void ConfigureColor(IDD_CX_CLIENT_CONFIG& config);
EVT_IDD_CX_DEVICE_IO_CONTROL DeviceControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED AdapterReady;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN AssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN UnassignSwapChain;
} // namespace display
