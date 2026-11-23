#pragma once

#include "IddController.h"
#include "ProfileReady.h"
#include <cfgmgr32.h>
#include <memory>
#include <optional>
#include <system_error>

namespace profiles {
using InterfaceSet = std::unique_ptr<void, decltype(&SetupDiDestroyDeviceInfoList)>;

inline InterfaceSet DisplayInterfaces()
{
    const auto set = OpenDisplayInterfaceSet();
    if (set == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "Enumerate display interfaces");
    return {set, SetupDiDestroyDeviceInfoList};
}

inline DWORD ReadAdapterInstance(HDEVINFO set, DWORD index, WCHAR (&id)[MAX_DEVICE_ID_LEN])
{
    SP_DEVICE_INTERFACE_DATA entry{sizeof(entry)};
    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_IDD_DRIVER_DEVICE, index, &entry))
        return GetLastError();
    SP_DEVINFO_DATA device{sizeof(device)};
    DWORD required{};
    // The sizing call also returns the devnode that owns this exact interface.
    if (!SetupDiGetDeviceInterfaceDetailW(set, &entry, nullptr, 0, &required, &device)) {
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER) return error;
    }
    return SetupDiGetDeviceInstanceIdW(set, &device, id, ARRAYSIZE(id), nullptr) ? ERROR_SUCCESS : GetLastError();
}

template<class Visit> void VisitAdapterInstances(HDEVINFO set, Visit visit)
{
    for (DWORD index = 0;; ++index) {
        WCHAR id[MAX_DEVICE_ID_LEN]{};
        const DWORD error = ReadAdapterInstance(set, index, id);
        if (error == ERROR_NO_MORE_ITEMS) return;
        if (InterfaceWasRemoved(error)) continue;
        if (error) throw std::system_error(error, std::system_category(), "Read virtual adapter instance ID");
        if (!visit(index, id)) return;
    }
}

inline std::optional<DWORD> FindAdapterIndex(HDEVINFO set, const wchar_t* wanted)
{
    if (!wanted) return {};
    std::optional<DWORD> selected;
    VisitAdapterInstances(set, [&](DWORD index, const wchar_t* id) {
        if (CompareStringOrdinal(wanted, -1, id, -1, TRUE) != CSTR_EQUAL) return true;
        selected = index;
        return false;
    });
    if (!selected) throw std::system_error(ERROR_NOT_FOUND, std::system_category(), "Find virtual adapter instance");
    return selected;
}
} // namespace profiles
