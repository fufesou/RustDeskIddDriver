#include "Driver.h"
#include "DeviceControl.tmh"

namespace display {
template<class T> static const T* Input(const InputBuffer& buffer)
{
    return buffer.bytes >= sizeof(T) ? static_cast<const T*>(buffer.data) : nullptr;
}

NTSTATUS Device::Control(ULONG code, InputBuffer input, OutputBuffer& output)
{
    std::lock_guard<std::mutex> guard(controls);
    const bool legacy = DEVICE_TYPE_FROM_CTL_CODE(code) == IOCTL_CHANGER_BASE;
    const NTSTATUS pending = legacy ? STATUS_ERROR_ADAPTER_NOT_INIT : STATUS_DEVICE_NOT_READY;
    if (!adapter) return pending;
    auto* state = GetAdapterContext(adapter);
    if (!InterlockedCompareExchange(&state->initFinished, 0, 0)) return pending;
    if (!NT_SUCCESS(state->initStatus)) return state->initStatus;
    switch (code) {
    case IOCTL_IDD_APPLY_PROFILE: return ApplyRequest(input);
    case IOCTL_IDD_DISCONNECT_PROFILE: return DisconnectProfile(input);
    case IOCTL_IDD_SESSION_PROFILE:
    case IOCTL_IDD_SESSION_MODES: return SessionRequest(code, input);
    case IOCTL_IDD_UPDATE_PROFILE_MODES: return UpdateProfileModes(input);
    case IOCTL_IDD_SET_RENDER_ADAPTER: {
        if (input.bytes == sizeof(LUID)) return SetGpu(*Input<LUID>(input));
        if (input.bytes != sizeof(CtlNamedRenderAdapter))
            return input.bytes < sizeof(LUID) ? STATUS_BUFFER_TOO_SMALL : STATUS_INVALID_PARAMETER;
        const auto& request = *Input<CtlNamedRenderAdapter>(input);
        const NTSTATUS status = MatchMonitor(request.Monitor);
        return NT_SUCCESS(status) ? SetGpu(request.RenderAdapter) : status;
    }
    case IOCTL_IDD_QUERY_CONNECTOR: return ReadConnector(input, output);
    case IOCTL_IDD_QUERY_CURSOR:
    case IOCTL_IDD_QUERY_COLOR: return ReadOutput(code, input, output);
    default: return LegacyControl(code, input);
    }
}

NTSTATUS Device::ApplyRequest(InputBuffer input)
{
    auto p = Input<CtlDisplayProfile>(input);
    if (!p) return STATUS_BUFFER_TOO_SMALL;
    auto profile = std::make_shared<Profile>();
    NTSTATUS status = DecodeProfile(*p, input.bytes, *profile);
    profile->timings.ForEachTimingIssue([](const CtlDisplayMode& mode, const char* reason) {
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE, "EDID timing %ux%u@%u is unusable: %s",
            mode.Width, mode.Height, mode.Numerator, reason);
    });
    return NT_SUCCESS(status) ? Apply(std::move(profile)) : status;
}

NTSTATUS Device::DisconnectProfile(InputBuffer input)
{
    const auto* expected = Input<CtlProfileDisconnect>(input);
    if (!expected) return STATUS_BUFFER_TOO_SMALL;
    const UINT index = expected->Monitor.ConnectorIndex;
    if (input.bytes != sizeof(*expected) || index >= IDD_MAX_CONNECTORS ||
        IsEqualGUID(expected->Monitor.ContainerId, GUID_NULL)) return STATUS_INVALID_PARAMETER;
    if (!slots[index].handle) return expected->Arrival ? STATUS_RETRY : STATUS_SUCCESS;
    if (!expected->Arrival || expected->Arrival != arrivals[index] || !NT_SUCCESS(MatchMonitor(expected->Monitor)))
        return STATUS_RETRY;
    return Detach(index);
}

NTSTATUS Device::SessionRequest(ULONG code, InputBuffer input)
{
    const auto* session = Input<CtlSessionProfile>(input);
    if (!session) return STATUS_BUFFER_TOO_SMALL;
    const auto& profile = session->Profile;
    if (profile.Version != IDD_PROFILE_VERSION) return STATUS_REVISION_MISMATCH;
    if (profile.ConnectorIndex >= IDD_MAX_CONNECTORS || IsEqualGUID(session->AttachmentId, GUID_NULL))
        return STATUS_INVALID_PARAMETER;
    const InputBuffer payload{&profile, input.bytes - offsetof(CtlSessionProfile, Profile)};
    if (!PayloadFits(profile, payload.bytes)) return STATUS_INVALID_PARAMETER;
    const bool modes = code == IOCTL_IDD_SESSION_MODES;
    const bool create = !modes && (profile.Flags & IDD_PROFILE_ENABLED);
    auto& slot = slots[profile.ConnectorIndex];
    // Control holds the device lock through the empty-slot check and publication.
    if (slot.handle && (create || !IsEqualGUID(slot.attachment, session->AttachmentId)))
        return STATUS_OBJECT_NAME_COLLISION;
    if (modes) return UpdateProfileModes(payload);
    const NTSTATUS status = ApplyRequest(payload);
    if (create && NT_SUCCESS(status)) slot.attachment = session->AttachmentId;
    return status;
}

NTSTATUS Device::UpdateProfileModes(InputBuffer input)
{
    const auto* p = Input<CtlDisplayProfile>(input);
    if (!p) return STATUS_BUFFER_TOO_SMALL;
    if (p->ConnectorIndex >= IDD_MAX_CONNECTORS) return STATUS_ERROR_INDEX_OOR;
    const NTSTATUS status = ValidateModeRequest(*p, input.bytes, slots[p->ConnectorIndex].profile);
    if (!NT_SUCCESS(status)) return status;
    const auto* modes = reinterpret_cast<const CtlDisplayMode*>(p + 1);
    return ChangeModes(p->ConnectorIndex, Modes(modes, modes + p->ModeCount), ModeRequest::Profile);
}

NTSTATUS Device::LegacyControl(ULONG code, InputBuffer input)
{
    switch (code) {
    case IOCTL_CHANGER_IDD_PLUG_IN: {
        auto p = Input<CtlPlugIn>(input);
        if (!p) return STATUS_BUFFER_TOO_SMALL;
        if (p->ConnectorIndex >= IDD_MAX_CONNECTORS) return STATUS_ERROR_INDEX_OOR;
        if (slots[p->ConnectorIndex].handle) return STATUS_ERROR_MONITOR_EXISTS;
        return Attach(LegacyProfile(*p));
    }
    case IOCTL_CHANGER_IDD_PLUG_OUT: {
        auto p = Input<CtlPlugOut>(input);
        if (!p) return STATUS_BUFFER_TOO_SMALL;
        return p->ConnectorIndex < IDD_MAX_CONNECTORS ? Detach(p->ConnectorIndex) : STATUS_ERROR_INDEX_OOR;
    }
    case IOCTL_CHANGER_IDD_UPDATE_MONITOR_MODE: {
        auto p = Input<CtlMonitorModes>(input);
        return p ? Update(*p, input.bytes) : STATUS_BUFFER_TOO_SMALL;
    }
    default: return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS Device::MatchMonitor(const CtlMonitorIdentity& monitor) const
{
    if (monitor.ConnectorIndex >= IDD_MAX_CONNECTORS) return STATUS_INVALID_PARAMETER;
    const auto& slot = slots[monitor.ConnectorIndex];
    return slot.handle && IsEqualGUID(monitor.ContainerId, slot.profile->header.ContainerId) ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

NTSTATUS Device::ReadConnector(InputBuffer input, OutputBuffer& output) const
{
    const auto index = Input<UINT>(input);
    if (!index || input.bytes != sizeof(UINT) || *index >= IDD_MAX_CONNECTORS) return STATUS_INVALID_PARAMETER;
    const auto& slot = slots[*index];
    if (!slot.handle) return STATUS_NOT_FOUND;
    if (output.size < sizeof(GUID)) return STATUS_BUFFER_TOO_SMALL;
    const CtlConnectorState state{slot.profile->header.ContainerId, arrivals[*index]};
    output.used = output.size >= sizeof(state) ? sizeof(state) : sizeof(state.ContainerId);
    memcpy(output.data, &state, output.used);
    return STATUS_SUCCESS;
}

NTSTATUS Device::ReadOutput(ULONG code, InputBuffer input, OutputBuffer& output)
{
    const auto index = Input<UINT>(input);
    if (!index || *index >= IDD_MAX_CONNECTORS) return STATUS_INVALID_PARAMETER;
    const auto& slot = slots[*index];
    const auto handle = slot.handle;
    const bool color = code == IOCTL_IDD_QUERY_COLOR;
    const bool named = input.bytes == (color ? sizeof(CtlColorQuery) : sizeof(CtlNamedCursorQuery));
    const size_t connectorBytes = color ? sizeof(UINT) : sizeof(CtlCursorQuery);
    if (!named && input.bytes != connectorBytes)
        return input.bytes < connectorBytes ? STATUS_BUFFER_TOO_SMALL : STATUS_INVALID_PARAMETER;
    if (named) {
        const NTSTATUS status = MatchMonitor(*Input<CtlMonitorIdentity>(input));
        if (!NT_SUCCESS(status)) return status;
    }
    if (!handle) return STATUS_ERROR_MONITOR_NOT_EXISTS;
    if (color) return GetMonitor(handle).color.Read(output, named ? &slot.target : nullptr);
    // Buffered I/O may alias input and output.
    const CtlCursorQuery copy{*index, named ? Input<CtlNamedCursorQuery>(input)->LastShapeId : Input<CtlCursorQuery>(input)->LastShapeId};
    return GetMonitor(handle).ReadCursor(copy, output);
}

_Use_decl_annotations_
void DeviceControl(WDFDEVICE device, WDFREQUEST request, size_t outBytes, size_t inBytes, ULONG code)
{
    void* input{};
    void* output{};
    NTSTATUS status = WdfRequestRetrieveInputBuffer(request, 1, &input, &inBytes);
    if (NT_SUCCESS(status) && outBytes)
        status = WdfRequestRetrieveOutputBuffer(request, 1, &output, &outBytes);
    OutputBuffer result{static_cast<BYTE*>(output), outBytes};
    try {
        if (NT_SUCCESS(status))
            status = GetDeviceContext(device)->instance->Control(code, {input, inBytes}, result);
    } catch (const std::bad_alloc&) {
        status = STATUS_INSUFFICIENT_RESOURCES;
    } catch (const std::length_error&) {
        status = STATUS_INVALID_PARAMETER;
    }
    if (!NT_SUCCESS(status))
        TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "IOCTL %lu failed %!STATUS!", code, status);
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? result.used : 0);
}
} // namespace display
