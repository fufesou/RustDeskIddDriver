#pragma once

#include "AdapterInstances.h"
#include "DisplayCommands.h"
#include "ProfileReady.h"
#include <wrl/wrappers/corewrappers.h>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace profiles {
using Microsoft::WRL::Wrappers::FileHandle;

enum class DeviceUse { Existing, Apply, Disconnect, Unique };

struct DeviceSelection {
    DeviceUse use;
    std::optional<DWORD> index;
};

struct ConnectorMatch {
    DWORD error;
    UINT64 arrival = 0;
};

struct ProfileDevices {
    const DeviceUse use;
    FileHandle owner{};
    UINT64 ownerArrival = 0;
    std::vector<FileHandle> empty;
    bool pending = false;
    bool occupied = false;
    bool ownerElsewhere = false;

    void Record(FileHandle device, ConnectorMatch match, bool selected = true) {
        const DWORD status = match.error;
        if (!selected) {
            // An explicit apply only needs to know if another instance owns the profile.
            ownerElsewhere |= status == ERROR_SUCCESS;
            return;
        }
        if (status == ERROR_NOT_FOUND) {
            empty.push_back(std::move(device));
            return;
        }
        if (status == ERROR_ALREADY_EXISTS) {
            occupied = true;
            return;
        }
        if (status == ERROR_NOT_READY) {
            pending = true;
            return;
        }
        if (InterfaceWasRemoved(status)) return;
        if (status) throw std::system_error(status, std::system_category(), "Query display connector");
        if (owner.IsValid()) throw std::system_error(ERROR_DUP_NAME, std::system_category(),
            "Multiple display adapters match the target. Select an adapter instance or a unique monitor profile");
        ownerArrival = match.arrival;
        owner = std::move(device);
    }

    template<class Probe> ConnectorMatch ConfirmMatch(HANDLE device, ConnectorMatch match, Probe probe) {
        const auto confirm = [&]() -> RequestResult {
            const auto previous = probe(owner.Get());
            if (previous.error) {
                Record(std::move(owner), previous);
                return {};
            }
            const auto current = probe(device);
            // A continuous attachment on either handle proves the two observations overlapped.
            const bool bothChanged = previous.arrival != ownerArrival && current.arrival != match.arrival;
            ownerArrival = previous.arrival;
            match = current;
            if (!current.error && bothChanged) return {ERROR_RETRY, false, true};
            return {};
        };
        const auto result = RequestWhenReady(confirm, GetTickCount64, Sleep);
        if (result.error) throw std::system_error(result.error, std::system_category(), "Monitor moved between adapters");
        return match;
    }

    bool AllowsEmpty() const { return use == DeviceUse::Apply || use == DeviceUse::Disconnect; }

    bool HasCandidate() const {
        if (owner.IsValid()) return use != DeviceUse::Unique || !pending;
        if (empty.empty()) return false;
        return use == DeviceUse::Apply || (use == DeviceUse::Disconnect && !pending);
    }

    void Check() const {
        if (ownerElsewhere && !owner.IsValid())
            throw std::system_error(ERROR_ALREADY_EXISTS, std::system_category(), "Profile belongs to another adapter instance");
        if (HasCandidate()) return;
        if (pending)
            throw std::system_error(ERROR_NOT_READY, std::system_category(), "Display adapter is initializing");
        if (AllowsEmpty() && occupied)
            throw std::system_error(ERROR_ALREADY_EXISTS, std::system_category(), "Connector is occupied on every available adapter");
        throw std::system_error(AllowsEmpty() ? ERROR_NO_MORE_ITEMS : ERROR_NOT_FOUND,
            std::system_category(), "Find a display adapter for the profile connector and container ID");
    }

    RequestResult CheckOwnerAfterFailure(DWORD error, ConnectorMatch current) const {
        if (!current.error) return {error, false, current.arrival != ownerArrival};
        if (current.error == ERROR_NOT_FOUND || current.error == ERROR_ALREADY_EXISTS || WaitingForDevice({current.error, true}))
            return {error, false, true};
        return {current.error, false};
    }

    static constexpr DWORD NoMode = static_cast<DWORD>(ERROR_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED);
    static bool NeedsProbe(DWORD error) { return error == ERROR_ALREADY_EXISTS || error == NoMode; }

    static RequestResult CheckEmptyAfterFailure(DWORD error, DWORD current, bool automaticApply) {
        if (current == ERROR_ALREADY_EXISTS || (automaticApply && current == ERROR_SUCCESS)) return {error, false, true};
        if (current == ERROR_NOT_FOUND || current == ERROR_SUCCESS) return {error, false};
        return {current, false, InterfaceWasRemoved(current)};
    }

    template<class Send, class Probe> RequestResult SubmitEmpty(Send& send, Probe& probe, bool automaticApply) const {
        bool waiting = pending;
        for (const auto& device : empty) {
            const DWORD error = send(device.Get());
            RequestResult result{error, false, InterfaceWasRemoved(error)};
            if (NeedsProbe(error)) {
                const DWORD current = probe(device.Get()).error;
                if (current == ERROR_NOT_FOUND && error == NoMode) continue;
                result = CheckEmptyAfterFailure(error, current, automaticApply);
            }
            if (result.error != ERROR_NOT_READY || use != DeviceUse::Apply) return result;
            waiting = true;
        }
        return {waiting ? ERROR_NOT_READY : NoMode, false};
    }

    template<class Send, class Probe> RequestResult Submit(Send send, Probe probe, bool automaticApply = false) const {
        if (!owner.IsValid()) return SubmitEmpty(send, probe, automaticApply);
        const DWORD error = send(owner.Get());
        if (automaticApply && NeedsProbe(error)) return CheckOwnerAfterFailure(error, probe(owner.Get()));
        return {error, false, InterfaceWasRemoved(error)};
    }
};

template<class Open, class Probe>
ProfileDevices SelectProfileDevice(Open open, Probe probe, DeviceSelection selection)
{
    ProfileDevices devices{selection.use};
    const bool single = selection.index && !devices.AllowsEmpty();
    for (DWORD offset = 0; !single || offset == 0; ++offset) {
        const DWORD index = single ? *selection.index : offset;
        FileHandle device;
        const auto opened = open(index, device.GetAddressOf());
        if (!opened) break;
        const DWORD error = *opened;
        const bool selected = !selection.index || index == *selection.index;
        if ((!selected && error) || InterfaceWasRemoved(error)) continue;
        if (error && error != ERROR_NOT_READY) throw std::system_error(error, std::system_category(), "Open display interface");
        auto match = error ? ConnectorMatch{error} : probe(device.Get());
        if (selected && !match.error && devices.owner.IsValid())
            match = devices.ConfirmMatch(device.Get(), match, probe);
        devices.Record(std::move(device), match, selected);
    }
    devices.Check();
    return devices;
}

struct CaptureBuffer {
    void* data;
    DWORD size;
    DWORD used = 0;
};

inline DWORD QueryCaptureColor(HANDLE device, CtlColorQuery query, CaptureBuffer& output)
{
    return DeviceIoControl(device, IOCTL_IDD_QUERY_COLOR, &query, sizeof(query), output.data, output.size,
                           &output.used, nullptr) ? ERROR_SUCCESS : GetLastError();
}

template<class Probe> ProfileDevices OpenMatchingDevice(Probe probe, DeviceUse use, const wchar_t* instance = nullptr)
{
    const auto set = DisplayInterfaces();
    const DeviceSelection selection{use, FindAdapterIndex(set.get(), instance)};
    const auto open = [&](DWORD index, HANDLE* device) -> std::optional<DWORD> {
        SP_DEVICE_INTERFACE_DATA entry{sizeof(entry)};
        if (SetupDiEnumDeviceInterfaces(set.get(), nullptr, &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, index, &entry))
            return OpenDisplayInterface(set.get(), &entry, device);
        const DWORD error = GetLastError();
        if (error == ERROR_NO_MORE_ITEMS) return std::nullopt;
        if (InterfaceWasRemoved(error)) return error;
        throw std::system_error(error, std::system_category(), "Enumerate display interfaces");
    };
    return SelectProfileDevice(open, probe, selection);
}

inline ConnectorMatch ProbeConnector(HANDLE device, UINT connector, const GUID* expected)
{
    CtlConnectorState owner{};
    DWORD used{};
    if (!DeviceIoControl(device, IOCTL_IDD_QUERY_CONNECTOR, &connector, sizeof(connector),
                         &owner, sizeof(owner), &used, nullptr)) return {GetLastError()};
    if (used != sizeof(owner)) return {ERROR_INVALID_DATA};
    if (expected && !IsEqualGUID(*expected, owner.ContainerId)) return {ERROR_ALREADY_EXISTS};
    return {ERROR_SUCCESS, owner.Arrival};
}

inline FileHandle OpenConnector(UINT connector, const GUID* expected, DeviceUse use)
{
    return OpenMatchingDevice([&](HANDLE device) { return ProbeConnector(device, connector, expected); }, use).owner;
}

inline FileHandle OpenProfileDevice(CtlMonitorIdentity monitor)
{
    return OpenConnector(monitor.ConnectorIndex, &monitor.ContainerId, DeviceUse::Existing);
}

inline FileHandle OpenLiveControl(const commands::DriverRequest& request)
{
    const auto input = request.input.data;
    const auto size = request.input.bytes;
    switch (request.code) {
    case IOCTL_IDD_SET_RENDER_ADAPTER:
        if (size == sizeof(LUID))
            return OpenMatchingDevice([](HANDLE) { return ConnectorMatch{ERROR_SUCCESS}; }, DeviceUse::Unique, request.adapterId).owner;
        if (size == sizeof(CtlNamedRenderAdapter))
            return OpenProfileDevice(static_cast<const CtlNamedRenderAdapter*>(input)->Monitor);
        break;
    case IOCTL_IDD_QUERY_CURSOR:
        if (size == sizeof(CtlCursorQuery)) return OpenConnector(static_cast<const CtlCursorQuery*>(input)->ConnectorIndex, nullptr, DeviceUse::Unique);
        if (size == sizeof(CtlNamedCursorQuery))
            return OpenProfileDevice(static_cast<const CtlNamedCursorQuery*>(input)->Monitor);
        break;
    case IOCTL_IDD_QUERY_COLOR:
        if (size == sizeof(UINT)) return OpenConnector(*static_cast<const UINT*>(input), nullptr, DeviceUse::Unique);
        if (size == sizeof(CtlColorQuery)) return OpenProfileDevice(*static_cast<const CtlColorQuery*>(input));
        break;
    }
    throw std::system_error(ERROR_INVALID_PARAMETER, std::system_category(), "Invalid display control request");
}

template<class Send> RequestResult DisconnectProfile(const ProfileDevices& devices, const CtlDisplayProfile& profile, Send send)
{
    const auto confirm = [&](HANDLE device, UINT64 arrival) -> RequestResult {
        CtlProfileDisconnect expected{{profile.ConnectorIndex, profile.ContainerId}, arrival};
        const DWORD error = send(device, {IOCTL_IDD_DISCONNECT_PROFILE, {&expected, sizeof(expected)}});
        return {error, false, error == ERROR_RETRY || InterfaceWasRemoved(error)};
    };
    if (devices.owner.IsValid()) return confirm(devices.owner.Get(), devices.ownerArrival);
    for (const auto& device : devices.empty) {
        const auto result = confirm(device.Get(), 0);
        if (result.error) return result;
    }
    return {};
}

template<class Send> RequestResult SendControl(const commands::DriverRequest& request, Send send)
{
    const auto code = request.code;
    const auto input = request.input.data;
    const auto size = request.input.bytes;
    if (!input) throw std::invalid_argument("Missing display control input");
    const auto submit = [&](HANDLE device) { return send(device, request); };
    const CtlDisplayProfile* profile;
    switch (code) {
    case IOCTL_IDD_APPLY_PROFILE:
    case IOCTL_IDD_UPDATE_PROFILE_MODES:
        if (size < sizeof(CtlDisplayProfile)) throw std::invalid_argument("Incomplete display profile");
        profile = static_cast<const CtlDisplayProfile*>(input);
        break;
    case IOCTL_IDD_SESSION_PROFILE:
    case IOCTL_IDD_SESSION_MODES:
        if (size < sizeof(CtlSessionProfile)) throw std::invalid_argument("Incomplete session profile");
        profile = &static_cast<const CtlSessionProfile*>(input)->Profile;
        break;
    default:
        return {submit(OpenLiveControl(request).Get()), false};
    }
    auto use = DeviceUse::Existing;
    if (code == IOCTL_IDD_APPLY_PROFILE || code == IOCTL_IDD_SESSION_PROFILE)
        use = profile->Flags & IDD_PROFILE_ENABLED ? DeviceUse::Apply : DeviceUse::Disconnect;
    const auto probe = [&](HANDLE device) { return ProbeConnector(device, profile->ConnectorIndex, &profile->ContainerId); };
    auto devices = OpenMatchingDevice(probe, use, request.adapterId);
    if (code == IOCTL_IDD_APPLY_PROFILE && use == DeviceUse::Disconnect)
        return DisconnectProfile(devices, *profile, send);
    const bool automaticApply = code == IOCTL_IDD_APPLY_PROFILE && !request.adapterId;
    const auto result = devices.Submit(submit, probe, automaticApply);
    if (!automaticApply || devices.owner.IsValid() || result.error != ERROR_ALREADY_EXISTS || result.reselect) return result;
    // A collision on a still-empty connector can also mean the profile appeared on another adapter.
    try {
        OpenMatchingDevice(probe, DeviceUse::Existing);
    } catch (const std::system_error& error) {
        const DWORD reason = static_cast<DWORD>(error.code().value());
        if (reason == ERROR_NOT_FOUND) return result;
        if (WaitingForDevice({reason, true})) return {reason, true};
        throw;
    }
    return {result.error, false, true};
}
} // namespace profiles
