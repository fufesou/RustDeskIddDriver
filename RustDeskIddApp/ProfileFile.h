#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../RustDeskIddDriver/ProfileValidation.h"
#include <filesystem>
#include <string>

namespace profiles {
constexpr CtlDisplayProfile DefaultHeader{IDD_PROFILE_VERSION, 0, {}, IDD_PROFILE_ENABLED};
struct File {
    CtlDisplayProfile header = DefaultHeader;
    std::vector<CtlDisplayMode> modes;
    std::vector<BYTE> edid;
};

UINT Number(const std::string& text);
CtlMonitorIdentity LoadIdentity(const std::filesystem::path& path);
CtlDisplayProfile LoadCaptureHeader(const std::filesystem::path& path);
File Load(const std::filesystem::path& path);
void Create(const std::filesystem::path& path, UINT connector);
std::vector<BYTE> Packet(const File& file, const GUID* attachmentId = nullptr);
} // namespace profiles
