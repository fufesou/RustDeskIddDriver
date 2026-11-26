/*++

Copyright (c) Microsoft Corporation

Abstract:

    This module contains a sample implementation of an indirect display driver. See the included README.md file and the
    various TODO blocks throughout this file and all accompanying files for information on building a production driver.

    MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

Environment:

    User Mode, UMDF

    For tracing log, use Inflight Trace Record (IFR) https://docs.microsoft.com/en-us/windows-hardware/drivers/wdf/using-wpp-software-tracing-in-kmdf-and-umdf-2-drivers
    // https://docs.microsoft.com/en-us/windows-hardware/drivers/devtest/adding-wpp-software-tracing-to-a-windows-driver#step-5-instrument-the-driver-code-to-generate-trace-messages-at-appropriate-points
--*/

#include "Driver.h"
#include "Driver.tmh"

const GUID GUID_DEVINTERFACE_IDD_DRIVER_DEVICE = \
    { 0x781EF630, 0x72B2, 0x11d2, { 0xB8, 0x52,  0x00,  0xC0,  0x4E,  0xAF,  0x52,  0x72 } };

using namespace display;

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
EVT_WDF_DEVICE_D0_ENTRY PowerOn;
EVT_WDF_DRIVER_UNLOAD Unload;

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, AddDevice);
    config.EvtDriverUnload = Unload;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DriverContext);
    attr.EvtDestroyCallback = [](WDFOBJECT obj) { delete GetDriverContext(obj)->directory; };
    auto directory = std::unique_ptr<DescriptorDirectory>(new (std::nothrow) DescriptorDirectory);
    if (!directory) return STATUS_INSUFFICIENT_RESOURCES;
    WPP_INIT_TRACING(object, path);
    WDFDRIVER driver;
    NTSTATUS status = WdfDriverCreate(object, path, &attr, &config, &driver);
    if (NT_SUCCESS(status)) GetDriverContext(driver)->directory = directory.release();
    else WPP_CLEANUP(object);
    return status;
}

_Use_decl_annotations_
void Unload(WDFDRIVER driver)
{
    UNREFERENCED_PARAMETER(driver);
    WPP_CLEANUP(WdfDriverWdmGetDriverObject(driver));
}

static NTSTATUS CreateModeWorkItem(WDFDEVICE device)
{
    WDF_WORKITEM_CONFIG work;
    WDF_WORKITEM_CONFIG_INIT(&work, [](WDFWORKITEM item) {
        auto owner = reinterpret_cast<WDFDEVICE>(WdfWorkItemGetParentObject(item));
        GetDeviceContext(owner)->instance->RefreshRenderModes();
    });
    work.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    auto* context = GetDeviceContext(device);
    NTSTATUS status = WdfWorkItemCreate(&work, &attr, &context->modeWork);
    if (!NT_SUCCESS(status)) return status;

    WDF_TIMER_CONFIG retry;
    WDF_TIMER_CONFIG_INIT(&retry, [](WDFTIMER timer) {
        WdfWorkItemEnqueue(reinterpret_cast<WDFWORKITEM>(WdfTimerGetParentObject(timer)));
    });
    retry.AutomaticSerialization = FALSE;
    attr.ParentObject = context->modeWork;
    return WdfTimerCreate(&retry, &attr, &context->modeRetry);
}

_Use_decl_annotations_
NTSTATUS AddDevice(WDFDRIVER, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = PowerOn;
    power.EvtDeviceReleaseHardware = [](WDFDEVICE device, WDFCMRESLIST) {
        GetDeviceContext(device)->instance->ReleaseAdapter();
        return STATUS_SUCCESS;
    };
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxDeviceIoControl = DeviceControl;
    config.EvtIddCxAdapterInitFinished = AdapterReady;
    config.EvtIddCxMonitorAssignSwapChain = AssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = UnassignSwapChain;
    ConfigureModes(config);
    ConfigureColor(config);
    NTSTATUS status = IddCxDeviceInitConfig(init, &config);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DeviceContext);
    attr.EvtCleanupCallback = [](WDFOBJECT obj) { delete GetDeviceContext(obj)->instance; };
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &attr, &device);
    if (!NT_SUCCESS(status)) return status;
    auto* context = new (std::nothrow) Device(device);
    if (!context) return STATUS_INSUFFICIENT_RESOURCES;
    GetDeviceContext(device)->instance = context;
    status = CreateModeWorkItem(device);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, nullptr);
    return NT_SUCCESS(status) ? IddCxDeviceInitialize(device) : status;
}

_Use_decl_annotations_
NTSTATUS PowerOn(WDFDEVICE device, WDF_POWER_DEVICE_STATE)
{
    return GetDeviceContext(device)->instance->InitAdapter();
}

namespace display {
static void SetRestartAdapter(WDFDEVICE device, IDDCX_ADAPTER adapter)
{
    auto* owner = GetDeviceContext(device);
    AcquireSRWLockExclusive(&owner->restartLock);
    owner->restartAdapter = adapter;
    ReleaseSRWLockExclusive(&owner->restartLock);
}

static void ReportInitFailure(IDDCX_ADAPTER adapter, AdapterContext& state)
{
    if (!InterlockedCompareExchange(&state.initFinished, 0, 0) || NT_SUCCESS(state.initStatus)) return;
    auto device = static_cast<WDFDEVICE>(InterlockedCompareExchangePointer(&state.parentDevice, nullptr, nullptr));
    if (!device) return; // InitAdapter checks again after publishing the parent.
    auto* owner = GetDeviceContext(device);
    AcquireSRWLockExclusive(&owner->restartLock);
    if (owner->restartAdapter == adapter) {
        owner->restartAdapter = nullptr;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Adapter initialization failed %!STATUS!", state.initStatus);
        WdfDeviceSetFailed(device, WdfDeviceFailedAttemptRestart);
    }
    ReleaseSRWLockExclusive(&owner->restartLock);
}

void Device::ReleaseAdapter()
{
    std::lock_guard<std::mutex> guard(controls);
    if (!adapter) return;
    SetRestartAdapter(device, nullptr);
    // IddCx owns the handles. The WDF device can outlive its stopped adapter.
    for (auto& slot : slots) {
        if (slot.profile) Descriptors().Remove(slot.profile);
        slot = {};
    }
    adapter = nullptr;
    preferredGpu = {};
}

NTSTATUS Device::InitAdapter()
{
    std::lock_guard<std::mutex> guard(controls);
    if (adapter) {
        auto* state = GetAdapterContext(adapter);
        return InterlockedCompareExchange(&state->initFinished, 0, 0) ? state->initStatus : STATUS_SUCCESS;
    }
    IDDCX_ENDPOINT_VERSION version = {};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = IDD_MAX_CONNECTORS;
#if IDDCX_VERSION_MINOR >= 10
    if (AdvancedColorAvailable()) caps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;
#endif
    auto& diag = caps.EndPointDiagnostics;
    diag.Size = sizeof(diag);
    diag.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    diag.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    diag.pEndPointFriendlyName = L"RustDesk Idd Device";
    diag.pEndPointManufacturerName = L"RustDesk";
    diag.pEndPointModelName = L"RustDesk Idd Model";
    diag.pFirmwareVersion = &version;
    diag.pHardwareVersion = &version;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, AdapterContext);
    IDARG_IN_ADAPTER_INIT input = {};
    input.WdfDevice = device;
    input.pCaps = &caps;
    input.ObjectAttributes = &attr;
    IDARG_OUT_ADAPTER_INIT result = {};
    NTSTATUS status = IddCxAdapterInitAsync(&input, &result);
    if (NT_SUCCESS(status)) {
        adapter = result.AdapterObject;
        auto* state = GetAdapterContext(adapter);
        SetRestartAdapter(device, adapter);
        InterlockedExchangePointer(&state->parentDevice, device);
        ReportInitFailure(adapter, *state);
    }
    return status;
}

_Use_decl_annotations_
NTSTATUS AdapterReady(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* input)
{
    auto* context = GetAdapterContext(adapter);
    InterlockedExchange(&context->initStatus, input->AdapterInitStatus);
    InterlockedExchange(&context->initFinished, TRUE);
    ReportInitFailure(adapter, *context);
    return STATUS_SUCCESS;
}
} // namespace display
