#include "Driver.h"
#include "EdidTiming.h"

namespace display {
namespace {
const BYTE DellEdid[] = {
        0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x10,0xAC,0xE6,0xD0,0x55,0x5A,0x4A,0x30,0x24,0x1D,0x01,
            0x04,0xA5,0x3C,0x22,0x78,0xFB,0x6C,0xE5,0xA5,0x55,0x50,0xA0,0x23,0x0B,0x50,0x54,0x00,0x02,0x00,
            0xD1,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x58,0xE3,0x00,
            0xA0,0xA0,0xA0,0x29,0x50,0x30,0x20,0x35,0x00,0x55,0x50,0x21,0x00,0x00,0x1A,0x00,0x00,0x00,0xFF,
            0x00,0x37,0x4A,0x51,0x58,0x42,0x59,0x32,0x0A,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,
            0x53,0x32,0x37,0x31,0x39,0x44,0x47,0x46,0x0A,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFD,0x00,0x28,
            0x9B,0xFA,0xFA,0x40,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x2C
};
const BYTE LenovoEdid[] = {
        0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00,0x30,0xAE,0xBF,0x65,0x01,0x01,0x01,0x01,0x20,0x1A,0x01,
            0x04,0xA5,0x3C,0x22,0x78,0x3B,0xEE,0xD1,0xA5,0x55,0x48,0x9B,0x26,0x12,0x50,0x54,0x00,0x08,0x00,
            0xA9,0xC0,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x68,0xD8,0x00,
            0x18,0xF1,0x70,0x2D,0x80,0x58,0x2C,0x45,0x00,0x53,0x50,0x21,0x00,0x00,0x1E,0x00,0x00,0x00,0x10,
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFD,0x00,
            0x30,0x92,0xB4,0xB4,0x22,0x01,0x0A,0x20,0x20,0x20,0x20,0x20,0x20,0x00,0x00,0x00,0xFC,0x00,0x4C,
            0x45,0x4E,0x20,0x59,0x32,0x37,0x66,0x41,0x0A,0x20,0x20,0x20,0x00,0x11
};
const std::vector<CtlDisplayMode> DellModes = {{2560,1440,144,1}, {1920,1080,60,1}, {1024,768,60,1}};
const std::vector<CtlDisplayMode> LenovoModes = {{3840,2160,60,1}, {1600,900,60,1}, {1024,768,60,1}};
const std::vector<CtlDisplayMode> NoEdidModes = {{1920,1080,60,1}, {1600,900,60,1}, {1024,768,75,1}};

} // namespace

DISPLAYCONFIG_VIDEO_SIGNAL_INFO Timing(const CtlDisplayMode& mode, bool monitor, const Profile& profile)
{
    auto signal = profile.timings.Signal(mode, !profile.legacy && !profile.edid.empty());
    signal.AdditionalSignalInfo.vSyncFreqDivider = monitor ? 0 : 1;
    signal.AdditionalSignalInfo.videoStandard = 255; // Unspecified video standard.
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return signal;
}

IDDCX_MONITOR_MODE Profile::DescribeMode(const CtlDisplayMode& mode) const
{
    IDDCX_MONITOR_MODE result{};
    result.Size = sizeof(result);
    const bool described = (legacy && !edid.empty()) || timings.Find(mode);
    result.Origin = described ? IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR : IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
    result.MonitorVideoSignalInfo = Timing(mode, true, *this);
    return result;
}

static auto ScanTimingKey(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal)
{
    return std::make_tuple(SignalKey(signal), signal.pixelRate, signal.vSyncFreq.Numerator, signal.vSyncFreq.Denominator);
}

TargetModes MatchTargetModes(const Profile& profile, std::shared_ptr<const Modes> modes)
{
    const bool fromProfile = modes.get() == &profile.modes;
    std::set<decltype(ScanTimingKey({}))> advertised;
    if (!fromProfile)
        for (const auto& mode : profile.modes) advertised.insert(ScanTimingKey(Timing(mode, true, profile)));

    UINT minimum = 0;
    for (const auto& mode : *modes) {
        const UINT extent = (std::max)(mode.Width, mode.Height);
        if (minimum && extent >= minimum) continue;
        if (fromProfile || advertised.count(ScanTimingKey(Timing(mode, false, profile)))) minimum = extent;
    }
    return {std::move(modes), minimum};
}

static auto MonitorModeKey(const Profile& profile, const CtlDisplayMode& mode)
{
    const auto description = profile.DescribeMode(mode);
    const auto& signal = description.MonitorVideoSignalInfo;
    return std::make_tuple(description.Origin, ScanTimingKey(signal), signal.videoStandard, signal.scanLineOrdering);
}

static bool SameMonitorDescription(const Profile& a, const Profile& b)
{
    return a.Hdr() == b.Hdr() && std::equal(a.modes.begin(), a.modes.end(), b.modes.begin(), b.modes.end(),
        [&](const auto& x, const auto& y) { return MonitorModeKey(a, x) == MonitorModeKey(b, y); });
}

void IdentifyEdid(Profile& profile)
{
    if (profile.edid.empty()) return;
    // The Windows parser has no connector handle. Give the description a stable serial per monitor.
    UINT serial = 2166136261u;
    const auto* id = reinterpret_cast<const BYTE*>(&profile.header.ContainerId);
    for (size_t i = 0; i < sizeof(GUID); ++i) serial = (serial ^ id[i]) * 16777619u;
    memcpy(profile.edid.data() + 12, &serial, sizeof(serial));
    FixChecksum(profile.edid.data());
}

const std::vector<CtlDisplayMode>& LegacyTargets()
{
    static const std::vector<CtlDisplayMode> modes = {
        {3840,2160,60,1}, {2560,1440,144,1}, {2560,1440,90,1}, {2560,1440,60,1},
        {1920,1080,144,1}, {1920,1080,90,1}, {1920,1080,60,1}, {1600,900,60,1},
        {1024,768,75,1}, {1024,768,60,1}
    };
    return modes;
}

std::shared_ptr<Profile> LegacyProfile(const CtlPlugIn& request)
{
    auto p = std::make_shared<Profile>();
    p->header = {IDD_PROFILE_VERSION, request.ConnectorIndex, request.ContainerId, IDD_PROFILE_ENABLED};
    p->legacy = true;
    if (request.MonitorEDID == MONITOR_EDID_MOD_DELL_S2719DGF) {
        p->edid.assign(std::begin(DellEdid), std::end(DellEdid));
        p->modes = DellModes;
    } else if (request.MonitorEDID == MONITOR_EDID_MOD_LENOVO_Y27fA) {
        p->edid.assign(std::begin(LenovoEdid), std::end(LenovoEdid));
        p->modes = LenovoModes;
    } else {
        p->modes = NoEdidModes;
    }
    return p;
}

NTSTATUS ValidateProfileSignals(const Profile& p, const Modes& modes)
{
    const auto status = p.timings.Validate(modes, !p.edid.empty() && !p.legacy).status;
    if (status == SignalListStatus::Duplicate) return STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET;
    if (status == SignalListStatus::RequiresYCbCr420 || status == SignalListStatus::UnusableEdidTiming ||
        status == SignalListStatus::InvalidDetailedSync)
        return STATUS_NOT_SUPPORTED;
    return status == SignalListStatus::Valid ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS GenerateEdid(Profile& p)
{
    p.edid = p.timings.Generate(p.modes, p.Hdr());
    if (p.edid.empty()) return STATUS_NOT_SUPPORTED;
    IdentifyEdid(p);
    p.generated = true;
    return ValidateProfileSignals(p, p.modes);
}

NTSTATUS DecodeProfile(const CtlDisplayProfile& request, size_t bytes, Profile& p)
{
    constexpr UINT flags = IDD_PROFILE_ENABLED | IDD_PROFILE_HDR | IDD_PROFILE_CURSOR;
    if (request.Version != IDD_PROFILE_VERSION) return STATUS_REVISION_MISMATCH;
    if (request.ConnectorIndex >= IDD_MAX_CONNECTORS || (request.Flags & ~flags) ||
        IsEqualGUID(request.ContainerId, GUID_NULL) || !PayloadFits(request, bytes))
        return STATUS_INVALID_PARAMETER;
    p.header = request;
    p.generated = false;
    if (!(request.Flags & IDD_PROFILE_ENABLED))
        return request.ModeCount || request.EdidBytes ? STATUS_INVALID_PARAMETER : STATUS_SUCCESS;
    const auto* modes = reinterpret_cast<const CtlDisplayMode*>(&request + 1);
    p.modes.assign(modes, modes + request.ModeCount);
    if (ValidateModes(p.modes) != ModeListStatus::Valid) return STATUS_INVALID_PARAMETER;
    const auto* data = reinterpret_cast<const BYTE*>(modes + request.ModeCount);
    if (request.EdidBytes) {
        if (request.EdidBytes > EdidBlock * (1u + UCHAR_MAX)) return STATUS_INVALID_PARAMETER;
        p.edid.assign(data, data + request.EdidBytes);
        if (!p.timings.Read(p.edid))
            return CheckEdidHeader(p.edid) == EdidHeaderStatus::UnsupportedVersion ? STATUS_REVISION_MISMATCH : STATUS_INVALID_PARAMETER;
        if (p.Hdr() && !p.timings.HasHdr10()) return STATUS_INVALID_PARAMETER;
        return ValidateProfileSignals(p, p.modes);
    }
    return GenerateEdid(p);
}

NTSTATUS ValidateModeRequest(const CtlDisplayProfile& request, size_t bytes, const ProfilePtr& current)
{
    if (request.Version != IDD_PROFILE_VERSION) return STATUS_REVISION_MISMATCH;
    if (request.Flags != IDD_PROFILE_ENABLED || !request.ModeCount || request.EdidBytes ||
        IsEqualGUID(request.ContainerId, GUID_NULL) || !PayloadFits(request, bytes)) return STATUS_INVALID_PARAMETER;
    if (!current) return STATUS_ERROR_MONITOR_NOT_EXISTS;
    return IsEqualGUID(current->header.ContainerId, request.ContainerId) ? STATUS_SUCCESS : STATUS_OBJECT_NAME_COLLISION;
}

bool SameModes(const Modes& a, const Modes& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const CtlDisplayMode& x, const CtlDisplayMode& y) { return ModeKey(x) == ModeKey(y); });
}

bool SameProfile(const Profile& a, const Profile& b)
{
    return a.legacy == b.legacy && a.generated == b.generated && a.header.Flags == b.header.Flags &&
           a.edid == b.edid && SameModes(a.modes, b.modes);
}

NTSTATUS DescriptorDirectory::Add(const ProfilePtr& profile, const ProfilePtr& replacing)
{
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& entry : entries) {
        const auto& other = entry.profile;
        if (other == replacing) continue;
        if (!IsEqualGUID(profile->header.ContainerId, GUID_NULL) &&
            IsEqualGUID(other->header.ContainerId, profile->header.ContainerId)) return STATUS_OBJECT_NAME_COLLISION;
        if (profile->edid.empty() || other->edid != profile->edid) continue;
        if (!SameMonitorDescription(*other, *profile)) return STATUS_OBJECT_NAME_COLLISION;
    }
    entries.push_back({profile, !replacing});
    return STATUS_SUCCESS;
}

void DescriptorDirectory::SwitchActive(const ProfilePtr& from, const ProfilePtr& to)
{
    std::lock_guard<std::mutex> guard(lock);
    for (auto& entry : entries) {
        if (entry.profile == from) entry.active = false;
        if (entry.profile == to) entry.active = true;
    }
}

void DescriptorDirectory::Remove(const ProfilePtr& profile)
{
    std::lock_guard<std::mutex> guard(lock);
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&](const Entry& entry) { return entry.profile == profile; }), entries.end());
}

ProfilePtr DescriptorDirectory::Find(const IDDCX_MONITOR_DESCRIPTION& desc)
{
    if (desc.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID || !desc.DataSize || !desc.pData) return {};
    std::lock_guard<std::mutex> guard(lock);
    for (const auto& entry : entries) {
        const auto& p = entry.profile;
        if (entry.active && p->edid.size() == desc.DataSize && !memcmp(p->edid.data(), desc.pData, desc.DataSize))
            return p;
    }
    return {};
}
} // namespace display
