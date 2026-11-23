#pragma once

#include "DescriptorTimings.h"
#include <memory>
#include <mutex>

namespace display {
struct Profile {
    CtlDisplayProfile header{};
    std::vector<CtlDisplayMode> modes;
    std::vector<BYTE> edid;
    DescriptorTimings timings;
    bool legacy = false;
    bool generated = false;

    bool Hdr() const { return (header.Flags & IDD_PROFILE_HDR) != 0; }
    bool Cursor() const { return (header.Flags & IDD_PROFILE_CURSOR) != 0; }
    IDDCX_MONITOR_MODE DescribeMode(const CtlDisplayMode& mode) const;
};

using ProfilePtr = std::shared_ptr<const Profile>;
std::shared_ptr<Profile> LegacyProfile(const CtlPlugIn& request);
const std::vector<CtlDisplayMode>& LegacyTargets();
DISPLAYCONFIG_VIDEO_SIGNAL_INFO Timing(const CtlDisplayMode& mode, bool monitor, const Profile& profile);
NTSTATUS DecodeProfile(const CtlDisplayProfile& request, size_t bytes, Profile& profile);
NTSTATUS ValidateModeRequest(const CtlDisplayProfile& request, size_t bytes, const ProfilePtr& current);
NTSTATUS GenerateEdid(Profile& profile);
NTSTATUS ValidateProfileSignals(const Profile& profile);
void IdentifyEdid(Profile& profile);
enum class ModeRequest { Profile, TargetList };
bool SameModes(const std::vector<CtlDisplayMode>& a, const std::vector<CtlDisplayMode>& b);
bool CanUpdateTargets(const std::vector<CtlDisplayMode>& advertised, const std::vector<CtlDisplayMode>& requested,
                      ModeRequest kind);
bool SameProfile(const Profile& a, const Profile& b);

// ParseMonitorDescription supplies only descriptor bytes. Keep advertised
// descriptors discoverable from before arrival until departure completes.
class DescriptorDirectory {
public:
    NTSTATUS Add(const ProfilePtr& profile, const ProfilePtr& replacing = {});
    void SwitchActive(const ProfilePtr& from, const ProfilePtr& to);
    void Remove(const ProfilePtr& profile);
    ProfilePtr Find(const IDDCX_MONITOR_DESCRIPTION& description);

private:
    std::mutex lock;
    struct Entry {
        ProfilePtr profile;
        bool active;
    };
    std::vector<Entry> entries;
};
} // namespace display
