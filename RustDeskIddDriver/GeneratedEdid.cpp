#include "DescriptorTimings.h"

namespace display {
void FixChecksum(BYTE* block)
{
    BYTE sum = 0;
    for (size_t i = 0; i < EdidChecksum; ++i) sum += block[i];
    block[EdidChecksum] = static_cast<BYTE>(0u - sum);
}

namespace {
constexpr size_t FirstDetailed = 54;
constexpr size_t DescriptorSize = 18;
constexpr BYTE MonitorName = 0xfc;
constexpr BYTE DummyDescriptor = 0x10;
constexpr BYTE VgaVic = 1, FirstDtdReference = 129, FirstVideoTimingReference = 145;

void Put16(BYTE* p, UINT value)
{
    p[0] = static_cast<BYTE>(value);
    p[1] = static_cast<BYTE>(value >> 8);
}

void BaseBlock(std::vector<BYTE>& bytes, bool native, bool hdr)
{
    const BYTE base[] = {
        0,255,255,255,255,255,255,0, 0x4a,0x74,1,0, 0,0,0,0, 0,34,1,4,
        static_cast<BYTE>(hdr ? 0xb2 : 0xa2),0,0,120,static_cast<BYTE>(native ? 6 : 4),
        0xee,0x91,0xa3,0x54,0x4c,0x99,0x26,0x0f,0x50,0x54
    };
    std::copy(std::begin(base), std::end(base), bytes.begin());
    std::fill_n(bytes.begin() + 38, 16, BYTE{1}); // Unused standard timing pairs.
    for (size_t offset = FirstDetailed; offset < ExtensionCount; offset += DescriptorSize)
        bytes[offset + 3] = DummyDescriptor;
    BYTE* name = bytes.data() + FirstDetailed + DescriptorSize;
    name[3] = MonitorName;
    const char text[] = "RustDesk IDD\n ";
    std::copy_n(text, 13, name + 5);
}

void DetailedTiming(BYTE* d, const CtlDisplayMode& m, const EdidRaster& r)
{
    Put16(d, r.clock);
    d[2] = static_cast<BYTE>(m.Width);
    d[3] = static_cast<BYTE>(r.hBlank);
    d[4] = static_cast<BYTE>(((m.Width >> 8) << 4) | (r.hBlank >> 8));
    d[5] = static_cast<BYTE>(m.Height);
    d[6] = static_cast<BYTE>(r.vBlank);
    d[7] = static_cast<BYTE>(((m.Height >> 8) << 4) | (r.vBlank >> 8));
    d[8] = static_cast<BYTE>(r.hFront);
    d[9] = static_cast<BYTE>(r.hSync);
    d[10] = static_cast<BYTE>(((r.vFront & 15) << 4) | (r.vSync & 15));
    d[11] = static_cast<BYTE>((r.hFront >> 8) << 6 | (r.hSync >> 8) << 4 |
                             (r.vFront >> 4) << 2 | (r.vSync >> 4));
    d[17] = 0x1e; // Digital separate sync, positive H/V.
}

BYTE* Extension(std::vector<BYTE>& edid)
{
    const size_t start = edid.size();
    edid.resize(start + EdidBlock);
    ++edid[ExtensionCount];
    return edid.data() + start;
}

struct CtaTiming {
    const CtlDisplayMode& mode;
    EdidRaster raster{};
    BYTE vic = 0;
    UINT clockKHz = 0;
    bool basePreferred = false;

    bool Prepare(const DetailedRaster* vga);

    BYTE PreferredReference() const {
        if (basePreferred) return FirstDtdReference;
        if (vic) return vic;
        return clockKHz ? FirstVideoTimingReference : 0;
    }
};

BYTE HighRefreshVic(const CtlDisplayMode& mode)
{
    for (const auto& v : CtaProgressiveTimings) {
        if (mode.Width != v.width || mode.Height != v.height) continue;
        const UINT64 nominal = UINT64(v.refresh) * mode.Denominator;
        if (mode.Numerator == nominal || (v.refresh % DualRateMultiple == 0 &&
            UINT64(mode.Numerator) * NtscDivisor == nominal * NtscScale)) return v.firstVic;
    }
    return 0;
}

struct ClockFormat {
    UINT unit, maxClock, maxBlank, minHz;
};
// EDID readers treat a base DTD below 10 MHz as invalid.
constexpr ClockFormat BaseClock{ClockUnitHz, USHRT_MAX, DtdMaxActive, 10000000};
constexpr ClockFormat ExtendedClock{TypeVIIClockHz, DisplayIdMaxClock, DisplayIdMaxActive, 0};
constexpr UINT BaseClockRangeKHz = (UINT(USHRT_MAX) + 1) * ClockUnitHz / TypeVIIClockHz;

UINT64 DivideRoundUp(UINT64 value, UINT64 divisor)
{
    return value / divisor + (value % divisor != 0);
}

UINT64 MinimumPixels(const CtlDisplayMode& mode, ClockFormat format)
{
    const UINT64 minimum = DivideRoundUp(UINT64(format.minHz) * mode.Denominator, mode.Numerator);
    const auto other = OtherBroadcastRate({mode.Numerator, mode.Denominator});
    if (!other.Numerator) return minimum;
    const UINT nominal = other.Denominator == 1 ? other.Numerator : mode.Numerator / mode.Denominator;
    // Separate the integer and fractional broadcast rates when the EDID is imported.
    const UINT64 overlap = UINT64(format.unit) * NtscDivisor / (2ull * nominal);
    return (std::max)(minimum, overlap + 1);
}

DISPLAYCONFIG_2DREGION PreferredTotals(DISPLAYCONFIG_2DREGION lower, DISPLAYCONFIG_2DREGION upper, UINT64 required)
{
    auto total = lower;
    while (UINT64(total.cx) * total.cy < required) {
        if (total.cx < upper.cx && (total.cx < total.cy || total.cy == upper.cy)) ++total.cx;
        else ++total.cy;
    }
    return total;
}

template<class Consider>
void TryAlignedTotals(DISPLAYCONFIG_2DREGION preferred, UINT64 step, const Consider& consider)
{
    if (UINT64(preferred.cx) * preferred.cy % step == 0 && consider(preferred.cx, preferred.cy)) return;
    const auto aligned = [&](UINT64 x, UINT64 y) {
        consider(DivideRoundUp(preferred.cx, x) * x, DivideRoundUp(preferred.cy, y) * y);
    };
    for (UINT64 x = 1; x <= step / x; ++x) {
        if (step % x) continue;
        aligned(x, step / x);
        aligned(step / x, x);
    }
}

bool FitClock(const CtlDisplayMode& mode, EdidRaster& raster, ClockFormat format)
{
    const UINT64 denominator = UINT64(mode.Denominator) * format.unit;
    const UINT64 divisor = Gcd(UINT64(mode.Numerator), denominator);
    // An exact clock advances by clockStep units for each group of pixelsPerStep pixels.
    const UINT64 pixelsPerStep = denominator / divisor, clockStep = mode.Numerator / divisor;
    const UINT maxWidth = mode.Width + format.maxBlank, maxHeight = mode.Height + format.maxBlank;
    const UINT minWidth = mode.Width + raster.hBlank, minHeight = mode.Height + raster.vBlank;
    const UINT64 required = (std::max)(UINT64(minWidth) * minHeight, MinimumPixels(mode, format));
    const UINT64 minSteps = DivideRoundUp(required, pixelsPerStep);
    const UINT64 maxSteps = (std::min)(UINT64(maxWidth) * maxHeight / pixelsPerStep, format.maxClock / clockStep);
    if (minSteps > maxSteps) return false;
    const UINT64 minimum = minSteps * pixelsPerStep, maximum = maxSteps * pixelsPerStep;
    UINT64 best = maximum + 1;
    const auto consider = [&](UINT64 w, UINT64 h) {
        if (w > maxWidth || h > maxHeight) return false;
        const UINT64 area = w * h;
        if (area >= best || !SignalRate(UINT64(mode.Numerator) * h, mode.Denominator).Numerator) return false;
        raster.hBlank = static_cast<UINT>(w) - mode.Width;
        raster.vBlank = static_cast<UINT>(h) - mode.Height;
        raster.clock = static_cast<UINT>(area / pixelsPerStep * clockStep);
        best = area;
        return true;
    };
    // Keep the established timing when the preferred blanking fits the clock.
    const auto preferred = PreferredTotals({minWidth, minHeight}, {maxWidth, maxHeight}, required);
    if (UINT64(preferred.cx) * preferred.cy <= maximum) TryAlignedTotals(preferred, pixelsPerStep, consider);
    if (best <= maximum) return true;
    // The clock bounds constrain the product. Either axis can supply the extra blanking.
    const UINT64 firstHeight = (std::max)(UINT64(minHeight), DivideRoundUp(minimum, maxWidth));
    for (UINT64 h = firstHeight; h <= maxHeight && UINT64(minWidth) * h < best; ++h) {
        const UINT64 alignment = pixelsPerStep / Gcd(pixelsPerStep, h);
        const UINT64 width = (std::max)(UINT64(minWidth), DivideRoundUp(minimum, h));
        consider(DivideRoundUp(width, alignment) * alignment, h);
        if (best == minimum) break;
    }
    return best <= maximum;
}

void AppendTypeVII(std::vector<BYTE>& blocks, const CtaTiming& video)
{
    constexpr BYTE TimingSize = 20, Revision = 2, UndefinedAspectRatio = 8;
    blocks.insert(blocks.end(), {0xe0 | (TimingSize + 2), TypeVIITimingTag, Revision});
    const size_t start = blocks.size();
    blocks.resize(start + TimingSize);
    BYTE* timing = blocks.data() + start;
    const auto& m = video.mode;
    const auto& r = video.raster;
    Put16(timing, video.clockKHz - 1);
    timing[2] = static_cast<BYTE>((video.clockKHz - 1) >> 16);
    timing[3] = UndefinedAspectRatio;
    Put16(timing + 4, m.Width - 1);
    Put16(timing + 6, r.hBlank - 1);
    Put16(timing + 8, (r.hFront - 1) | 0x8000);
    Put16(timing + 10, r.hSync - 1);
    Put16(timing + 12, m.Height - 1);
    Put16(timing + 14, r.vBlank - 1);
    Put16(timing + 16, (r.vFront - 1) | 0x8000);
    Put16(timing + 18, r.vSync - 1);
}

const BYTE* AppendCtaBlock(std::vector<BYTE>& edid, const CtaTiming& video, bool hdr)
{
    std::vector<BYTE> blocks;
    if (video.vic && video.vic != VgaVic) blocks = {0x42, video.vic, VgaVic};
    else blocks = {0x41, VgaVic};
    if (video.clockKHz) AppendTypeVII(blocks, video);
    const BYTE ref = video.PreferredReference();
    if (ref) blocks.insert(blocks.end(), {0xe2, 0x0d, ref}); // Video format preference.
    if (ref && !video.basePreferred) blocks.insert(blocks.end(), {0xe2, 0x08, ref}); // Native resolution.
    blocks.insert(blocks.end(), {0xe2, 0x00, 0x4a}); // Selectable RGB range, underscanned CE and IT.
    if (hdr) blocks.insert(blocks.end(), {0xe3, 0x05, 0x80, 0, 0xe3, HdrStaticMetadata, PqEotf | 1, StaticMetadataType1});
    BYTE* cta = Extension(edid);
    cta[0] = CtaExtension;
    cta[1] = 3;
    cta[3] = 0x80; // IT video formats are underscanned.
    constexpr BYTE CollectionStart = 4;
    cta[2] = static_cast<BYTE>(CollectionStart + blocks.size());
    std::copy(blocks.begin(), blocks.end(), cta + CollectionStart);
    FixChecksum(cta);
    return cta;
}

bool CtaTiming::Prepare(const DetailedRaster* vga)
{
    if (vga) {
        vic = VgaVic;
        // The fractional VIC 1 clock (25.175 MHz) falls between base DTD clock steps.
        basePreferred = vga->PixelRate() % ClockUnitHz == 0;
        return true;
    }
    if (!PreferredRaster(mode, raster)) return false;
    const bool baseSize = mode.Width <= DtdMaxActive && mode.Height <= DtdMaxActive;
    basePreferred = baseSize && FitClock(mode, raster, BaseClock);
    if (!basePreferred) {
        vic = HighRefreshVic(mode);
        if (!vic && FitClock(mode, raster, ExtendedClock) && (!baseSize || raster.clock > BaseClockRangeKHz))
            clockKHz = raster.clock;
    }
    return true;
}

void VgaBaseTiming(BYTE* dtd)
{
    // CTA VIC 1, 640x480 at exactly 60 Hz, 25.2 MHz with negative H/V sync.
    const BYTE vga[] = {0xd8,0x09,0x80,0xa0,0x20,0xe0,0x2d,0x10,0x10,0x60,0xa2,0,0,0,0,0,0,0x18};
    std::copy(std::begin(vga), std::end(vga), dtd);
}
} // namespace

std::vector<BYTE> DescriptorTimings::Generate(const std::vector<CtlDisplayMode>& modes, bool hdr)
{
    *this = {};
    if (modes.empty()) return {};
    NamedRasters vga;
    ReadVideoCodes(vga, &VgaVic, 1);
    CtaTiming first{modes.front()};
    if (!first.Prepare(FindNamed(vga, first.mode))) return {};
    std::vector<BYTE> edid(EdidBlock);
    BaseBlock(edid, first.basePreferred, hdr);
    bool hasBase = false;
    bool needsCta = hdr || (!first.basePreferred && (first.vic || first.clockKHz));
    for (const auto& mode : modes) {
        const bool preferred = &mode == &modes.front();
        CtaTiming timing = preferred ? first : CtaTiming{mode};
        if (!preferred && !timing.Prepare(FindNamed(vga, mode))) return {};
        const auto& r = timing.raster;
        if (!timing.basePreferred && !timing.vic && !timing.clockKHz) {
            const DISPLAYCONFIG_2DREGION total{mode.Width + r.hBlank, mode.Height + r.vBlank};
            driverModes.emplace(ModeKey(mode), GeneratedDriverSignal(mode, total));
            continue;
        }
        const bool base = !hasBase && timing.basePreferred;
        if (base) {
            if (timing.vic == VgaVic) VgaBaseTiming(edid.data() + FirstDetailed);
            else DetailedTiming(edid.data() + FirstDetailed, mode, timing.raster);
            hasBase = true;
        }
        if (timing.vic == VgaVic && !base) needsCta = true;
        const UINT unit = timing.basePreferred ? ClockUnitHz : TypeVIIClockHz;
        const DetailedRaster scan = timing.vic ? VideoRaster(*CtaShortVideoTiming(timing.vic), mode.Numerator % mode.Denominator != 0) :
            DetailedRaster{mode.Width + r.hBlank, mode.Height + r.vBlank, UINT64(r.clock) * unit, unit, 1, true};
        if (base || (preferred && timing.clockKHz)) videoModes.emplace(ModeKey(mode), scan);
        else driverModes.emplace(ModeKey(mode), scan.Signal(mode));
    }
    if (!hasBase) VgaBaseTiming(edid.data() + FirstDetailed);
    if (needsCta) {
        DescriptorTimings cta;
        const BYTE* block = AppendCtaBlock(edid, first, hdr);
        if (!cta.ReadCta(block)) return {};
        hdr10 = cta.HasHdr10();
        // The emitted video codes describe configured modes that have no explicit detailed timing.
        for (const auto& mode : cta.videoModes)
            if (driverModes.erase(mode.first)) videoModes.insert(mode);
    }
    FixChecksum(edid.data());
    return edid;
}
} // namespace display
