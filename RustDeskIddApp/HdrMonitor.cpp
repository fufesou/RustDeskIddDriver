#include "HdrSample.h"
#include <system_error>

namespace hdr {
namespace {
struct Search {
    const wchar_t* source;
    HMONITOR found{};
    DWORD error{};
};

BOOL CALLBACK MatchMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context)
{
    auto& search = *reinterpret_cast<Search*>(context);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        search.error = GetLastError();
        return FALSE;
    }
    if (_wcsicmp(info.szDevice, search.source)) return TRUE;
    search.found = monitor;
    return FALSE;
}

std::vector<DISPLAYCONFIG_PATH_INFO> ActivePaths()
{
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS;
    for (;;) {
        UINT32 pathCount{}, modeCount{};
        LONG status = GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS) throw std::system_error(status, std::system_category(), "Measure active display paths");
        if (!pathCount) return {};
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        status = QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status != ERROR_SUCCESS) throw std::system_error(status, std::system_category(), "Read active display paths");
        paths.resize(pathCount);
        return paths;
    }
}
} // namespace

DISPLAYCONFIG_SOURCE_DEVICE_NAME DisplaySourceName(const CtlDisplayTarget& target)
{
    const auto paths = ActivePaths();
    const auto* source = FindDisplaySource(paths, target);
    if (!source) throw std::runtime_error("The Windows display target has no active desktop monitor");
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), source->adapterId, source->id};
    const LONG status = DisplayConfigGetDeviceInfo(&name.header);
    if (status != ERROR_SUCCESS) throw std::system_error(status, std::system_category(), "Read Windows display source name");
    return name;
}

HMONITOR ActiveMonitor(const CtlDisplayTarget& target)
{
    const auto name = DisplaySourceName(target);
    Search search{name.viewGdiDeviceName};
    const BOOL complete = EnumDisplayMonitors(nullptr, nullptr, MatchMonitor, reinterpret_cast<LPARAM>(&search));
    if (search.found) return search.found;
    if (search.error) throw std::system_error(search.error, std::system_category(), "Read active monitor");
    if (!complete) throw std::system_error(GetLastError(), std::system_category(), "Enumerate active monitors");
    throw std::runtime_error("The Windows display target has no active desktop monitor");
}
} // namespace hdr
