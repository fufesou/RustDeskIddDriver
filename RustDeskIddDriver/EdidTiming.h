#pragma once
#include "ProfileValidation.h"

namespace display {
// The base EDID detailed timing has 12-bit active dimensions and a 16-bit
// clock. Extended timing descriptors have 16-bit dimensions and a 24-bit clock minus one.
constexpr UINT DtdMaxActive = 0xfff;
constexpr UINT DisplayIdMaxActive = 0x10000;
constexpr UINT ClockUnitHz = 10000;
constexpr UINT TypeVIIClockHz = 1000;
constexpr BYTE TypeVIITimingTag = 0x22;
constexpr UINT DisplayIdMaxClock = 0x1000000;
constexpr UINT NtscScale = 1000, NtscDivisor = 1001, DualRateMultiple = 6;

void FixChecksum(BYTE* block);

inline UINT EdidWord(const BYTE* data) { return data[0] | (UINT(data[1]) << 8); }

struct DetailedAxis {
    // Blanking and sync offset both start at the end of active video and include the leading border.
    UINT active, blank, offset, pulse, border;

    bool HasSync() const {
        // A pulse can start at the end of the leading border. Leave room for a back porch before the trailing border.
        return offset >= border && pulse && offset + pulse + border < blank;
    }
};

enum class DtdBorders { IncludedInBlanking, Separate };

inline DetailedAxis ReadDtdAxis(const BYTE* d, bool horizontal, DtdBorders borders)
{
    const BYTE* size = d + (horizontal ? 2 : 5);
    const UINT active = size[0] | (UINT(size[2] & 0xf0) << 4);
    const UINT blank = size[1] | (UINT(size[2] & 0x0f) << 8);
    const UINT offset = horizontal ? d[8] | (UINT(d[11] & 0xc0) << 2) :
                                     (d[10] >> 4) | (UINT(d[11] & 0x0c) << 2);
    const UINT pulse = horizontal ? d[9] | (UINT(d[11] & 0x30) << 4) :
                                    (d[10] & 0x0f) | (UINT(d[11] & 0x03) << 4);
    const UINT border = d[horizontal ? 15 : 16];
    // EDID 1.4 excludes the borders from the encoded blanking and front porch. Earlier revisions include them.
    const UINT excluded = borders == DtdBorders::Separate ? border : 0;
    return {active, blank + 2 * excluded, offset + excluded, pulse, border};
}

inline DetailedAxis ReadDisplayIdAxis(const BYTE* data)
{
    // Type I and VII encode each dimension minus one. Only the offset carries a polarity bit.
    return {EdidWord(data) + 1, EdidWord(data + 2) + 1, (EdidWord(data + 4) & 0x7fffu) + 1,
            EdidWord(data + 6) + 1, 0};
}

struct EdidRaster {
    UINT hBlank = 80, vBlank = 20;
    UINT hFront = 8, hSync = 32, vFront = 3, vSync = 5;
    UINT clock = 0; // Zero when the timing has no representable 10 kHz clock.
    BYTE vic = 0;
};

inline UINT64 RoundedEdidClock(const CtlDisplayMode& mode, DISPLAYCONFIG_2DREGION total, UINT unitHz)
{
    const UINT64 line = UINT64(mode.Numerator) * total.cy;
    const UINT64 unit = UINT64(mode.Denominator) * unitHz;
    // Split before multiplying by width. The undivided product can exceed 64 bits.
    return (line / unit) * total.cx + ((line % unit) * total.cx + unit / 2) / unit;
}

inline bool PreferredRaster(const CtlDisplayMode& mode, EdidRaster& raster)
{
    if (!mode.Width || !mode.Height || mode.Width > DisplayIdMaxActive ||
        mode.Height > DisplayIdMaxActive || !mode.Numerator || !mode.Denominator) return false;
    if (mode.Width == 3840 && mode.Height == 2160) {
        // CTA-861 VIC 117 and 118 use 5280x2250 and 4400x2250 totals.
        if (UINT64(mode.Numerator) == UINT64(100) * mode.Denominator) raster.vic = 117;
        if (mode.Numerator == 120 && mode.Denominator == 1 ||
            mode.Numerator == 120000 && mode.Denominator == 1001) raster.vic = 118;
        if (raster.vic) {
            raster.hBlank = raster.vic == 117 ? 1440 : 560;
            raster.hFront = raster.hBlank - 384;
            raster.hSync = 88;
            raster.vBlank = 90;
            raster.vFront = 8;
            raster.vSync = 10;
        }
    }
    const DISPLAYCONFIG_2DREGION total{mode.Width + raster.hBlank, mode.Height + raster.vBlank};
    const auto rounded = RoundedEdidClock(mode, total, ClockUnitHz);
    raster.clock = rounded <= DisplayIdMaxClock ? static_cast<UINT>(rounded) : 0;
    return true;
}
} // namespace display
