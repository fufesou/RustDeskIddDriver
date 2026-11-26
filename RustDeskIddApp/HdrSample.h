#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../RustDeskIddDriver/DisplayProfile.h"
#include <array>
#include <d3d11.h>
#include <memory>
#include <vector>

namespace hdr {
using Rgb = std::array<float, 3>;
using Codes = std::array<UINT, 3>;
struct Frame {
    UINT width, height;
    std::vector<std::array<USHORT, 4>> pixels;
};
struct Calibration {
    CtlColorState state{};
    UINT64 generation{};
    CtlDisplayTarget target{};
    LUID renderAdapter{};
    CtlColorTransform transform{};
    USHORT legacy[3][256]{};
};

inline bool SameAdapter(LUID a, LUID b)
{
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

inline bool SameTarget(const CtlDisplayTarget& a, const CtlDisplayTarget& b)
{
    return SameAdapter(a.AdapterLuid, b.AdapterLuid) && a.TargetId == b.TargetId;
}

inline const DISPLAYCONFIG_PATH_SOURCE_INFO* FindDisplaySource(const std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                                                              const CtlDisplayTarget& target)
{
    for (const auto& path : paths)
        if (SameTarget(target, {path.targetInfo.adapterId, path.targetInfo.id})) return &path.sourceInfo;
    return nullptr;
}

bool SameCalibration(const Calibration& a, const Calibration& b);
Codes Hdr10(const Rgb& scRgb, const Calibration& calibration);
Rgb ScRgb(const std::array<USHORT, 4>& half);
Frame ReadScRgbFrame(ID3D11Texture2D* texture);
DISPLAYCONFIG_SOURCE_DEVICE_NAME DisplaySourceName(const CtlDisplayTarget& target);
HMONITOR ActiveMonitor(const CtlDisplayTarget& target);
Frame CaptureFrame(HMONITOR monitor, LUID renderAdapter);
std::vector<BYTE> EncodePng(const Frame& frame, const Calibration& calibration);
void SavePng(const wchar_t* path, const std::vector<BYTE>& png);
} // namespace hdr
