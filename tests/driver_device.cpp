#include "../RustDeskIddDriver/Driver.h"
#include "../RustDeskIddApp/CaptureDevice.h"
#include <cassert>
#include <type_traits>
#include <utility>

using namespace display;

PFN_IDD_CX IddFunctions[IddFunctionTableNumEntries]{};
decltype(IddDriverGlobals) IddDriverGlobals{};
decltype(IddFrameworkExtensionName) IddFrameworkExtensionName{};
decltype(WdfDriverGlobals) WdfDriverGlobals{};
decltype(WdfFunctions) WdfFunctions{};
BOOLEAN IddClientVersionHigherThanFramework{}, WdfClientVersionHigherThanFramework{};
ULONG IddFunctionCount{}, IddStructureCount{}, WdfStructureCount{};
WDF_STRUCT_INFO IddStructures{}, WdfStructures{};

namespace {
// Use the production IOCTL dispatcher and replace only the WDF/IddCx callbacks.
struct Framework {
    AdapterContext adapter{TRUE, STATUS_SUCCESS};
    DeviceContext deviceContext{};
    MonitorContext monitor{};
    DescriptorDirectory directory;
    DriverContext driver{&directory};
    WDF_DRIVER_GLOBALS globals{reinterpret_cast<WDFDRIVER>(&driver)};
    WDFFUNC wdf[WdfFunctionTableNumEntries]{};
    bool rejectArrival = false, preferredRemoved = false;
    void (*onUpdate)(Monitor&) = nullptr;
    UINT busyGpu = 0;
    std::shared_ptr<const Modes> arrivalModes;
    Framework();
    ~Framework() {
        assert(!monitor.instance);
        WdfDriverGlobals = nullptr;
        WdfFunctions = nullptr;
        IddDriverGlobals = nullptr;
    }
    static Framework& Get(PIDD_DRIVER_GLOBALS id) { return *reinterpret_cast<Framework*>(id); }
    static void* FASTCALL Context(PWDF_DRIVER_GLOBALS, WDFOBJECT object, PCWDF_OBJECT_CONTEXT_TYPE_INFO) { return object; }
    static void Delete(PWDF_DRIVER_GLOBALS, WDFOBJECT object) {
        delete std::exchange(reinterpret_cast<MonitorContext*>(object)->instance, nullptr);
    }
    static NTSTATUS NTAPI Init(PIDD_DRIVER_GLOBALS id, const IDARG_IN_ADAPTER_INIT*, IDARG_OUT_ADAPTER_INIT* out) {
        out->AdapterObject = reinterpret_cast<IDDCX_ADAPTER>(&Get(id).adapter);
        return STATUS_SUCCESS;
    }
    static NTSTATUS NTAPI Create(PIDD_DRIVER_GLOBALS id, IDDCX_ADAPTER, const IDARG_IN_MONITORCREATE* in, IDARG_OUT_MONITORCREATE* out) {
        auto& monitor = Get(id).monitor;
        assert(in->pMonitorInfo->ConnectorIndex == 0 && !monitor.instance);
        out->MonitorObject = reinterpret_cast<IDDCX_MONITOR>(&monitor);
        return STATUS_SUCCESS;
    }
    static NTSTATUS NTAPI Arrive(PIDD_DRIVER_GLOBALS id, IDDCX_MONITOR monitor, IDARG_OUT_MONITORARRIVAL* out) {
        auto& fx = Get(id);
        if (std::exchange(fx.rejectArrival, false)) return STATUS_UNSUCCESSFUL;
        fx.arrivalModes = GetMonitor(monitor).Targets();
        assert(!fx.arrivalModes->empty());
        *out = {};
        return STATUS_SUCCESS;
    }
    static NTSTATUS NTAPI Depart(PIDD_DRIVER_GLOBALS, IDDCX_MONITOR monitor) {
        Delete(nullptr, reinterpret_cast<WDFOBJECT>(monitor));
        return STATUS_SUCCESS;
    }
    template<class T> static NTSTATUS NTAPI Updated(PIDD_DRIVER_GLOBALS id, IDDCX_MONITOR monitor, const T*) {
        if (auto action = std::exchange(Get(id).onUpdate, nullptr)) action(GetMonitor(monitor));
        return STATUS_SUCCESS;
    }
};

Framework::Framework() {
    WdfDriverGlobals = &globals;
    WdfFunctions = wdf;
    IddDriverGlobals = reinterpret_cast<PIDD_DRIVER_GLOBALS>(this);
    wdf[WdfObjectGetTypedContextWorkerTableIndex] = reinterpret_cast<WDFFUNC>(Context);
    wdf[WdfObjectDeleteTableIndex] = reinterpret_cast<WDFFUNC>(Delete);
    wdf[WdfWorkItemEnqueueTableIndex] = reinterpret_cast<WDFFUNC>(+[](PWDF_DRIVER_GLOBALS, WDFWORKITEM) {});
    IddFunctions[IddCxAdapterInitAsyncTableIndex] = reinterpret_cast<PFN_IDD_CX>(Init);
    IddFunctions[IddCxAdapterSetRenderAdapterTableIndex] = reinterpret_cast<PFN_IDD_CX>(
        +[](PIDD_DRIVER_GLOBALS, IDDCX_ADAPTER, const IDARG_IN_ADAPTERSETRENDERADAPTER*) {});
    IddFunctions[IddCxMonitorCreateTableIndex] = reinterpret_cast<PFN_IDD_CX>(Create);
    IddFunctions[IddCxMonitorArrivalTableIndex] = reinterpret_cast<PFN_IDD_CX>(Arrive);
    IddFunctions[IddCxMonitorDepartureTableIndex] = reinterpret_cast<PFN_IDD_CX>(Depart);
    IddFunctions[IddCxMonitorUpdateModesTableIndex] = reinterpret_cast<PFN_IDD_CX>(Updated<IDARG_IN_UPDATEMODES>);
#if IDDCX_VERSION_MINOR >= 10
    IddFunctions[IddCxMonitorUpdateModes2TableIndex] = reinterpret_cast<PFN_IDD_CX>(Updated<IDARG_IN_UPDATEMODES2>);
#endif
}

struct Packet {
    CtlSessionProfile header;
    CtlDisplayMode modes[2] = {{1920,1080,60,1}, {1280,720,50,1}};
    explicit Packet(UINT attachment) : header{{attachment}, {IDD_PROFILE_VERSION, 0, {42}, IDD_PROFILE_ENABLED, 2, 0}} {}
    NTSTATUS Send(Device& dev, ULONG code) const {
        const size_t size = sizeof(CtlDisplayProfile) + header.Profile.ModeCount * sizeof(modes[0]);
        OutputBuffer out{};
        if (code == IOCTL_IDD_APPLY_PROFILE) return dev.Control(code, {&header.Profile, size}, out);
        return dev.Control(code, {&header, offsetof(CtlSessionProfile, Profile) + size}, out);
    }
    Packet Disabled() const {
        Packet result = *this;
        result.header.Profile.Flags = result.header.Profile.ModeCount = 0;
        return result;
    }
};

static NTSTATUS TestRenderLimit(LUID gpu, UINT& limit) {
    const auto& fx = Framework::Get(IddDriverGlobals);
    if (gpu.LowPart && fx.busyGpu == gpu.LowPart) return STATUS_DEVICE_BUSY;
    if (gpu.LowPart && fx.preferredRemoved) return STATUS_NOT_FOUND;
    limit = gpu.LowPart ? gpu.LowPart : 8192;
    return STATUS_SUCCESS;
}

struct TestDevice : Device {
    OutputBuffer out{};
    explicit TestDevice(Framework& fx, UINT preferred = 0) : Device(reinterpret_cast<WDFDEVICE>(&fx.deviceContext), TestRenderLimit) {
        assert(IddMinimumVersionRequired == 4 && NT_SUCCESS(InitAdapter()));
        LUID gpu{preferred};
        assert(NT_SUCCESS(Control(IOCTL_IDD_SET_RENDER_ADAPTER, {&gpu, sizeof(gpu)}, out)));
    }
    NTSTATUS SetMode(CtlDisplayMode mode, CtlDisplayMode extra = {}) {
        struct { CtlMonitorModes first; decltype(CtlMonitorModes::Modes) second; } packet{
            {0, extra.Width ? 2u : 1u, {{mode.Width, mode.Height, mode.Numerator}}}, {{extra.Width, extra.Height, extra.Numerator}}};
        return Control(IOCTL_CHANGER_IDD_UPDATE_MONITOR_MODE, {&packet, CtlModeRequestBytes(packet.first.ModeCount)}, out);
    }
};

enum class Publication { WideOnly, Mixed, ReassignedDuringUpdate, Reexpanded };
void CheckReplacementRollback(UINT preferred, Publication publication = Publication::WideOnly, bool busy = false) {
    Framework fx;
    TestDevice device(fx, preferred);
    Packet profile(1);
    profile.modes[0] = {8193,1080,60,1};
    assert(NT_SUCCESS(profile.Send(device, IOCTL_IDD_APPLY_PROFILE)));
    assert(fx.arrivalModes->size() == 1 && fx.arrivalModes->front().Width == profile.modes[1].Width);
    assert(fx.monitor.instance->ObserveRenderLevel(D3D_FEATURE_LEVEL_11_0, {16384}));
    if (publication == Publication::ReassignedDuringUpdate)
        fx.onUpdate = [](Monitor& m) { m.ObserveRenderLevel(D3D_FEATURE_LEVEL_10_0, {8192}); };
    device.RefreshRenderModes();
    if (preferred) {
        profile.modes[1].Width = 1920;
        assert(NT_SUCCESS(profile.Send(device, IOCTL_IDD_APPLY_PROFILE)));
    }
    const bool keepBoth = publication != Publication::WideOnly;
    if (!keepBoth || publication == Publication::Reexpanded)
        assert(NT_SUCCESS(device.SetMode(profile.modes[keepBoth ? 1 : 0])));
    if (publication != Publication::ReassignedDuringUpdate)
        assert(fx.monitor.instance->ObserveRenderLevel(D3D_FEATURE_LEVEL_10_0, {8192}));
    if (keepBoth) {
        if (publication == Publication::Mixed) {
            fx.onUpdate = [](Monitor& m) { m.ReleaseRenderAssignment(); };
            assert(NT_SUCCESS(profile.Send(device, IOCTL_IDD_APPLY_PROFILE)));
        } else fx.monitor.instance->ReleaseRenderAssignment();
        device.RefreshRenderModes();
        if (publication == Publication::Reexpanded) assert(NT_SUCCESS(profile.Send(device, IOCTL_IDD_APPLY_PROFILE)));
        assert(!fx.onUpdate && fx.monitor.instance->Targets()->size() == 2);
    }
    profile.header.Profile.ModeCount = 1;
    profile.modes[0] = {1280,720,60,1};
    for (int attempt = 0; attempt < 2; ++attempt) {
        fx.rejectArrival = true;
        assert(profile.Send(device, IOCTL_IDD_APPLY_PROFILE) == STATUS_UNSUCCESSFUL);
        assert(fx.monitor.instance && fx.arrivalModes->size() == (keepBoth ? 2u : 1u) && fx.arrivalModes->front().Width == 8193);
    }
    if (keepBoth) {
        assert(NT_SUCCESS(profile.Disabled().Send(device, IOCTL_IDD_APPLY_PROFILE)));
        return;
    }
    fx.busyGpu = busy ? 16384 : 0;
    fx.preferredRemoved = !busy;
    fx.rejectArrival = true;
    const NTSTATUS expected = busy ? STATUS_DEVICE_BUSY : STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    assert(profile.Send(device, IOCTL_IDD_APPLY_PROFILE) == expected && !fx.monitor.instance);
}

void CheckRollbackWithBusyGpu(UINT busyGpu, bool keepMixed = false) {
    Framework fx;
    TestDevice device(fx, 8192);
    Packet profile(1), replacement(2);
    profile.modes[0] = {3840,2160,60,1};
    assert(NT_SUCCESS(profile.Send(device, IOCTL_IDD_APPLY_PROFILE)));
    if (!keepMixed) {
        fx.monitor.instance->ObserveRenderLevel(D3D_FEATURE_LEVEL_11_0, {16384});
        device.RefreshRenderModes();
        assert(NT_SUCCESS(device.SetMode(profile.modes[1])));
    }
    fx.busyGpu = busyGpu;
    fx.rejectArrival = true;
    assert(replacement.Send(device, IOCTL_IDD_APPLY_PROFILE) == STATUS_UNSUCCESSFUL);
    assert(fx.monitor.instance && fx.arrivalModes->size() == 1 && fx.arrivalModes->front().Width == 1280);
    const auto restored = fx.monitor.instance->QueryTargets();
    if (keepMixed) fx.monitor.instance->ObserveRenderLevel(D3D_FEATURE_LEVEL_11_0, {16384});
    assert(NT_SUCCESS(profile.Send(device, IOCTL_IDD_APPLY_PROFILE)) && fx.monitor.instance->Targets()->size() == 2);
    fx.onUpdate = [](Monitor& m) {
        m.ReleaseRenderAssignment();
        m.ObserveRenderLevel(D3D_FEATURE_LEVEL_9_1, {MinimumTextureDimension});
        IDD_CX_CLIENT_CONFIG config{};
        IDD_CX_CLIENT_CONFIG_INIT(&config);
        ConfigureModes(config);
        const auto check = [&](auto query, auto input) {
            IDARG_OUT_QUERYTARGETMODES out{};
            assert(query && NT_SUCCESS(query(m.handle, &input, &out)) && out.TargetModeBufferOutputCount == 1);
            std::remove_pointer_t<decltype(input.pTargetModes)> target{};
            input = {{}, 1, &target};
            assert(NT_SUCCESS(query(m.handle, &input, &out)) && out.TargetModeBufferOutputCount == 1);
            const auto expected = m.profile->DescribeMode({1280,720,50,1});
            assert(SignalKey(target.TargetVideoSignalInfo.targetVideoSignalInfo) == SignalKey(expected.MonitorVideoSignalInfo));
        };
#if IDDCX_VERSION_MINOR >= 10
        if (AdvancedColorAvailable()) return check(config.EvtIddCxMonitorQueryTargetModes2, IDARG_IN_QUERYTARGETMODES2{});
#endif
        check(config.EvtIddCxMonitorQueryTargetModes, IDARG_IN_QUERYTARGETMODES{});
    };
    assert(device.SetMode(profile.modes[0], {1600,1000,60,1}) == STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    device.RefreshRenderModes();
    assert(restored.limit == (keepMixed ? MinimumTextureDimension : (busyGpu == 16384 ? 8192u : 16384u)));
    assert(fx.monitor.instance && SameModes(*fx.monitor.instance->Targets(), restored.SourceModes()));
    assert(NT_SUCCESS(profile.Disabled().Send(device, IOCTL_IDD_APPLY_PROFILE)));
}

void CheckSessionOwner() {
    Framework fx;
    TestDevice device(fx);
    Packet owner(1), other(2);
    assert(NT_SUCCESS(owner.Send(device, IOCTL_IDD_SESSION_PROFILE)));
    assert(device.SetMode({2560,1440,60,1}) == STATUS_GRAPHICS_MODE_NOT_IN_MODESET && fx.monitor.instance->Targets() == fx.arrivalModes);
    assert(other.Send(device, IOCTL_IDD_SESSION_PROFILE) == STATUS_OBJECT_NAME_COLLISION);
    assert(other.Disabled().Send(device, IOCTL_IDD_SESSION_PROFILE) == STATUS_OBJECT_NAME_COLLISION);
    const CtlPlugIn plug{0, MONITOR_EDID_MOD_DELL_S2719DGF, owner.header.Profile.ContainerId};
    assert(fx.directory.Add(LegacyProfile({1, plug.MonitorEDID, plug.ContainerId})) == STATUS_OBJECT_NAME_COLLISION);
    CtlProfileDisconnect observed{{0, owner.header.Profile.ContainerId}, 1};
    const auto disconnect = [&] { return device.Control(IOCTL_IDD_DISCONNECT_PROFILE, {&observed, sizeof(observed)}, device.out); };
    assert(NT_SUCCESS(owner.Disabled().Send(device, IOCTL_IDD_SESSION_PROFILE)) && disconnect() == STATUS_RETRY);
    assert(NT_SUCCESS(device.Control(IOCTL_CHANGER_IDD_PLUG_IN, {&plug, sizeof(plug)}, device.out)) && disconnect() == STATUS_RETRY);
    const Modes mixed{{2560,1440,144,1}, {1600,1000,60,1}};
    assert(NT_SUCCESS(device.SetMode(mixed.front(), mixed.back())));
    for (const auto& unsupported : Modes{mixed.back(), {2560,1440,60,1}})
        assert(device.SetMode(unsupported) == STATUS_GRAPHICS_MODE_NOT_IN_MODESET && SameModes(*fx.monitor.instance->Targets(), mixed));
    ++observed.Arrival;
    assert(NT_SUCCESS(disconnect()) && !fx.monitor.instance);
}

static void CheckAllocationRaces() {
    const auto open = [](DWORD index, HANDLE* handle) -> std::optional<DWORD> {
        if (index < 2) return index ? ERROR_NOT_READY : ERROR_GEN_FAILURE;
        if (index > 3) return std::nullopt;
        *handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return *handle ? ERROR_SUCCESS : GetLastError();
    };
    const auto empty = [](HANDLE) { return profiles::ConnectorMatch{ERROR_NOT_FOUND}; };
    assert(profiles::SelectProfileDevice(open, empty, {profiles::DeviceUse::Apply, 2}).empty.size() == 1);
    const auto available = [&](DWORD i, HANDLE* h) { return open(i + 1, h); };
    try { profiles::SelectProfileDevice(available, empty, {profiles::DeviceUse::Disconnect}); assert(false); }
    catch (const std::system_error& error) { assert(error.code().value() == ERROR_NOT_READY); }
    const std::vector<profiles::ConnectorMatch> migration{{0,1}, {0,1}, {0,2}, {0,2}, {ERROR_NOT_FOUND}};
    size_t probes = 0;
    auto migrated = profiles::SelectProfileDevice(available, [&](HANDLE) { return migration.at(probes++); }, {profiles::DeviceUse::Apply});
    assert(migrated.owner.IsValid() && migrated.empty.size() == 1 && probes == migration.size());
    auto devices = profiles::SelectProfileDevice(available, empty, {profiles::DeviceUse::Apply});
    for (DWORD first : {ERROR_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED, ERROR_NOT_READY}) {
        unsigned attempts = 0;
        const auto send = [&](HANDLE h) { ++attempts; return h == devices.empty.front().Get() ? first : ERROR_SUCCESS; };
        assert(!devices.Submit(send, empty).error && attempts == 2);
    }
    const auto submit = [&](DWORD error, profiles::ConnectorMatch observed, bool automatic = false) {
        return devices.Submit([error](HANDLE) { return error; }, [observed](HANDLE) { return observed; }, automatic);
    };
    assert(submit(ERROR_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED, {ERROR_NOT_FOUND}).error == ERROR_NOT_READY);
    assert(submit(ERROR_ALREADY_EXISTS, {ERROR_ALREADY_EXISTS}).reselect);
    assert(submit(ERROR_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED, {ERROR_SUCCESS}, true).reselect);
    assert(submit(ERROR_DEVICE_REMOVED, {}).reselect && submit(ERROR_ALREADY_EXISTS, {ERROR_DEVICE_REMOVED}).reselect);
    for (DWORD status : {ERROR_NOT_FOUND, ERROR_SUCCESS, ERROR_ACCESS_DENIED}) {
        const auto result = submit(ERROR_ALREADY_EXISTS, {status});
        assert(!result.reselect && result.error == (status == ERROR_ACCESS_DENIED ? status : ERROR_ALREADY_EXISTS));
    }
    devices.owner = std::move(devices.empty.back());
    assert(submit(ERROR_DEVICE_REMOVED, {}).reselect);
    for (DWORD error : {ERROR_ALREADY_EXISTS, ERROR_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED}) {
        assert(!submit(error, {ERROR_ALREADY_EXISTS}).reselect);
        for (DWORD status : {ERROR_NOT_FOUND, ERROR_ALREADY_EXISTS, ERROR_SUCCESS}) {
            const auto result = submit(error, {status, devices.ownerArrival}, true);
            assert(result.error == error && result.reselect == (status != ERROR_SUCCESS));
        }
        assert(submit(error, {ERROR_SUCCESS, devices.ownerArrival + 1}, true).reselect);
    }
}
} // namespace

void CheckDriverDevice() {
    CheckSessionOwner();
    for (UINT preferred : {0u, 8192u}) CheckReplacementRollback(preferred);
    CheckReplacementRollback(8192, Publication::WideOnly, true);
    for (auto publication : {Publication::Mixed, Publication::ReassignedDuringUpdate, Publication::Reexpanded})
        CheckReplacementRollback(0, publication);
    for (UINT busy : {0u, 8192u, 16384u}) CheckRollbackWithBusyGpu(busy);
    CheckRollbackWithBusyGpu(8192, true);
    CheckAllocationRaces();
}
