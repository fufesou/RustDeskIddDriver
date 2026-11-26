#include "Driver.h"
#include "MonitorDevice.tmh"

namespace display {
Device::~Device()
{
    ReleaseAdapter();
}

static NTSTATUS CreateMonitor(IDDCX_ADAPTER adapter, const Profile& profile, IDDCX_MONITOR& handle)
{
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, MonitorContext);
    attr.EvtCleanupCallback = [](WDFOBJECT object) { delete GetMonitorContext(object)->instance; };
    IDDCX_MONITOR_INFO info = {};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
    info.MonitorContainerId = profile.header.ContainerId;
    info.ConnectorIndex = profile.header.ConnectorIndex;
    auto& desc = info.MonitorDescription;
    desc.Size = sizeof(desc);
    desc.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    desc.DataSize = static_cast<UINT>(profile.edid.size());
    desc.pData = profile.edid.empty() ? nullptr : const_cast<BYTE*>(profile.edid.data());
    IDARG_IN_MONITORCREATE input = {};
    input.ObjectAttributes = &attr;
    input.pMonitorInfo = &info;
    IDARG_OUT_MONITORCREATE result = {};
    NTSTATUS status = IddCxMonitorCreate(adapter, &input, &result);
    if (NT_SUCCESS(status)) handle = result.MonitorObject;
    return status;
}

static TargetModes ProfileTargets(const ProfilePtr& profile)
{
    return MatchTargetModes(*profile, {profile, profile->legacy ? &LegacyTargets() : &profile->modes});
}

NTSTATUS Device::Attach(ProfilePtr profile)
{
    for (const auto& slot : slots)
        if (slot.profile && IsEqualGUID(slot.profile->header.ContainerId, profile->header.ContainerId))
            return STATUS_OBJECT_NAME_COLLISION;
    InitialTargets initial{0, ProfileTargets(profile), preferredGpu};
    NTSTATUS status = InitialLimit(initial);
    if (!NT_SUCCESS(status)) return status;
    auto& directory = Descriptors();
    status = directory.Add(profile);
    if (!NT_SUCCESS(status)) return status;
    status = Arrive(profile, GUID_NULL, std::move(initial));
    if (!NT_SUCCESS(status)) directory.Remove(profile);
    return status;
}

NTSTATUS Device::Arrive(const ProfilePtr& profile, const GUID& attachment, InitialTargets initial)
{
    const UINT index = profile->header.ConnectorIndex;
    const UINT limit = initial.limit;
    if (!initial.requested.Fits(limit)) {
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Connector %u has no monitor-compatible target within %u pixels", index, limit);
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    std::unique_ptr<Monitor> monitor;
    try {
        monitor = std::make_unique<Monitor>(profile, std::move(initial), GetDeviceContext(device)->modeWork);
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (monitor->Targets()->size() != monitor->RequestedTargets().modes->size())
        TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Connector %u target modes limited to %u pixels per axis", index, limit);
    IDDCX_MONITOR handle{};
    NTSTATUS status = CreateMonitor(adapter, *profile, handle);
    if (NT_SUCCESS(status)) {
        monitor->handle = handle;
        GetMonitorContext(handle)->instance = monitor.release();
        IDARG_OUT_MONITORARRIVAL arrival = {};
        status = IddCxMonitorArrival(handle, &arrival);
        if (NT_SUCCESS(status)) {
            ++arrivals[index];
            slots[index] = {handle, profile, attachment, {arrival.OsAdapterLuid, arrival.OsTargetId}};
            return status;
        }
        WdfObjectDelete(handle);
    }
    return status;
}

NTSTATUS Device::Detach(UINT index)
{
    auto& slot = slots[index];
    if (!slot.handle) return STATUS_ERROR_MONITOR_NOT_EXISTS;
    NTSTATUS status = IddCxMonitorDeparture(slot.handle);
    if (NT_SUCCESS(status)) {
        Descriptors().Remove(slot.profile);
        slot = {};
    }
    return status;
}

NTSTATUS Device::Replace(UINT index, ProfilePtr next)
{
    auto old = slots[index].profile;
    const GUID attachment = slots[index].attachment;
    const auto& monitor = GetMonitor(slots[index].handle);
    auto targets = monitor.PrepareReplacement(ProfileTargets(next), preferredGpu);
    if (!targets.next.limit) {
        const NTSTATUS probe = InitialLimit(targets.next);
        if (!NT_SUCCESS(probe)) return probe;
    }
    if (!targets.next.requested.Fits(targets.next.limit)) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    auto& directory = Descriptors();
    NTSTATUS status = directory.Add(next, old);
    if (!NT_SUCCESS(status)) return status;
    status = IddCxMonitorDeparture(slots[index].handle);
    if (!NT_SUCCESS(status)) {
        directory.Remove(next);
        return status;
    }
    slots[index] = {};
    directory.SwitchActive(old, next);
    status = Arrive(next, attachment, std::move(targets.next));
    if (NT_SUCCESS(status)) {
        directory.Remove(old);
        return status;
    }
    directory.SwitchActive(next, old);
    directory.Remove(next);
    NTSTATUS rollback = RestoreRenderLimit(targets.previous);
    if (NT_SUCCESS(rollback)) rollback = Arrive(old, attachment, std::move(targets.previous));
    if (!NT_SUCCESS(rollback)) {
        directory.Remove(old);
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Replace failed %!STATUS!, restoring profile failed %!STATUS!", status, rollback);
        return rollback;
    }
    return status;
}

NTSTATUS Device::Apply(ProfilePtr profile)
{
    const UINT index = profile->header.ConnectorIndex;
    const bool enabled = (profile->header.Flags & IDD_PROFILE_ENABLED) != 0;
    const auto& current = slots[index].profile;
    if (enabled && profile->Hdr() && !AdvancedColorAvailable()) return STATUS_NOT_SUPPORTED;
    if (current && !IsEqualGUID(current->header.ContainerId, profile->header.ContainerId))
        return STATUS_OBJECT_NAME_COLLISION;
    if (!enabled) return current ? Detach(index) : STATUS_SUCCESS;
    if (!current) return Attach(std::move(profile));
    if (SameProfile(*current, *profile))
        return GetMonitor(slots[index].handle).PublishTargets(profile->modes, preferredGpu, probeLimit);
    return Replace(index, std::move(profile));
}

NTSTATUS Device::Update(const CtlMonitorModes& request, size_t bytes)
{
    const size_t header = offsetof(CtlMonitorModes, Modes);
    if (request.ConnectorIndex >= IDD_MAX_CONNECTORS) return STATUS_ERROR_INDEX_OOR;
    if (!request.ModeCount || request.ModeCount > (bytes - header) / sizeof(request.Modes[0]))
        return STATUS_INVALID_PARAMETER;
    const auto& slot = slots[request.ConnectorIndex];
    if (!slot.handle) return STATUS_ERROR_MONITOR_NOT_EXISTS;

    Modes modes;
    modes.reserve(request.ModeCount);
    for (UINT i = 0; i < request.ModeCount; ++i) {
        const auto& mode = request.Modes[i];
        modes.push_back({mode.Width, mode.Height, mode.Sync, 1});
    }
    return ChangeModes(request.ConnectorIndex, std::move(modes), ModeRequest::TargetList);
}

NTSTATUS Device::ChangeModes(UINT index, Modes modes, ModeRequest kind)
{
    const auto& slot = slots[index];
    if (ValidateModes(modes) != ModeListStatus::Valid) return STATUS_INVALID_PARAMETER;
    if (kind == ModeRequest::TargetList) {
        const NTSTATUS status = ValidateProfileSignals(*slot.profile, modes);
        if (!NT_SUCCESS(status)) return status;
    }
    if (kind == ModeRequest::TargetList || SameModes(slot.profile->modes, modes))
        return GetMonitor(slot.handle).PublishTargets(modes, preferredGpu, probeLimit);

    auto next = std::make_shared<Profile>(*slot.profile);
    if (next->legacy) {
        IdentifyEdid(*next);
        if (!next->timings.Read(next->edid)) return STATUS_INVALID_PARAMETER;
    }
    next->legacy = false;
    next->modes = std::move(modes);
    const NTSTATUS status = next->generated ? GenerateEdid(*next) : ValidateProfileSignals(*next, next->modes);
    if (!NT_SUCCESS(status)) return status;
    return Replace(index, std::move(next));
}

NTSTATUS Device::SetGpu(LUID luid)
{
    if (!IDD_IS_FUNCTION_AVAILABLE(IddCxAdapterSetRenderAdapter)) return STATUS_NOT_SUPPORTED;
    if (SameAdapter(luid, preferredGpu)) return STATUS_SUCCESS;
    UINT limit{};
    NTSTATUS status = probeLimit(luid, limit);
    if (!NT_SUCCESS(status)) return status;
    for (const auto& slot : slots) {
        if (!slot.handle) continue;
        auto& monitor = GetMonitor(slot.handle);
        if (!monitor.RequestedTargets().Fits(limit)) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
        // The outgoing preference may have published modes before Windows assigned a swap chain.
        monitor.RetainRenderPreference(preferredGpu);
    }
    bool changed = false;
    for (const auto& slot : slots)
        if (slot.handle && GetMonitor(slot.handle).PrepareRenderLimit(limit)) changed = true;
    IDARG_IN_ADAPTERSETRENDERADAPTER input = {};
    input.PreferredRenderAdapter = luid;
    preferredGpu = luid;
    IddCxAdapterSetRenderAdapter(adapter, &input);
    if (changed) WdfWorkItemEnqueue(GetDeviceContext(device)->modeWork);
    return STATUS_SUCCESS;
}

static bool RetryRenderUpdate(NTSTATUS status)
{
    return status == STATUS_INSUFFICIENT_RESOURCES || status == STATUS_NO_MEMORY || status == STATUS_DEVICE_BUSY ||
           status == STATUS_DEVICE_NOT_READY || status == STATUS_RETRY || status == STATUS_GRAPHICS_TRY_AGAIN_LATER ||
           status == STATUS_GRAPHICS_TRY_AGAIN_NOW;
}

NTSTATUS Device::RefreshRenderLimit(UINT index)
{
    auto& monitor = GetMonitor(slots[index].handle);
    RenderChange change{};
    if (!monitor.PendingRenderChange(change)) return STATUS_SUCCESS;
    if (monitor.RequestedTargets().Fits(change.limit)) {
        const auto previous = monitor.Targets();
        const NTSTATUS status = monitor.RefreshTargets(change);
        if (NT_SUCCESS(status) && previous != monitor.Targets())
            TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE, "Connector %u has %u usable targets at %u pixels per axis",
                        index, static_cast<UINT>(monitor.Targets()->size()), change.limit);
        return status;
    }
    // IddCx cannot publish an empty target mode list.
    if (!monitor.BeginWithdrawal(change)) return STATUS_SUCCESS;
    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Withdrawing connector %u: no configured mode fits the assigned GPU (%u pixels)", index, change.limit);
    const NTSTATUS status = Detach(index);
    if (!NT_SUCCESS(status)) monitor.CancelWithdrawal();
    return status;
}

void Device::RefreshRenderModes()
{
    std::lock_guard<std::mutex> guard(controls);
    bool retry = false;
    for (UINT i = 0; i < IDD_MAX_CONNECTORS; ++i) {
        if (!slots[i].handle) continue;
        const NTSTATUS status = RefreshRenderLimit(i);
        if (NT_SUCCESS(status)) continue;
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "Updating connector %u render limits failed %!STATUS!", i, status);
        retry = retry || RetryRenderUpdate(status);
    }
    if (retry) {
        constexpr ULONG RetryIntervalMs = 1000;
        WdfTimerStart(GetDeviceContext(device)->modeRetry, WDF_REL_TIMEOUT_IN_MS(RetryIntervalMs));
    }
}
} // namespace display
