#pragma once

#include "SignalTiming.h"
#include "DmtTimings.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace display {
namespace base_edid {
constexpr size_t EstablishedOffset = 35, StandardOffset = 38, DescriptorOffset = 54, DescriptorSize = 18;
constexpr BYTE AdditionalStandard = 0xfa, EstablishedIII = 0xf7, ThreeByteCvt = 0xf8, RangeLimits = 0xfd;
constexpr BYTE VideoTimingExtension = 0x10;
constexpr BYTE DefaultGtf = 0x00, LimitsOnly = 0x01, SecondaryGtf = 0x02, Cvt = 0x04;
constexpr UINT CellPixels = 8, CellPair = 2 * CellPixels, MinSyncBackPorchUs = 550, UsPerSecond = 1000000;
constexpr UINT CvtPorch = 3, CvtBackPorch = 7, ClockStepKHz = 250;
constexpr double DefaultDuty = 30.0, DutySlopePerUs = 0.3;

// A lower source rank, earlier record, then earlier choice within that record wins.
enum class Source { CvtCode, Standard, Established };
struct Origin { Source source; const BYTE* record; UINT choice = 0; };
enum class Formula { Gtf, Cvt, Unknown };
enum class Issue { None, GtfDuty, GtfRaster, CvtRaster, GtfSync, CvtSync, UnknownFormula, WindowsRate };
struct TimingRecord { VesaTiming timing; Origin origin; Issue issue; };

inline const char* TimingIssue(Issue issue)
{
    switch (issue) {
    case Issue::GtfDuty: return "GTF blanking duty cycle is outside its usable range";
    case Issue::GtfRaster: return "GTF cannot represent the raster or pixel clock";
    case Issue::CvtRaster: return "CVT cannot represent the raster or pixel clock";
    case Issue::GtfSync: return "GTF blanking cannot fit the sync pulse and porches";
    case Issue::CvtSync: return "CVT blanking cannot fit the sync pulse and porches";
    case Issue::UnknownFormula: return "unknown EDID range timing formula";
    case Issue::WindowsRate: return "Windows cannot represent the horizontal or vertical scan frequency";
    default: return "";
    }
}

class NamedTimings {
    using Key = decltype(ModeKey(CtlDisplayMode{}));
    using Priority = std::tuple<Source, size_t, UINT>;
    struct Candidate { Priority priority; DetailedRaster raster; };
    std::map<Key, Candidate> names;
    std::map<Key, std::set<Issue>> issues;

    void Register(const Key& key, const Candidate& candidate)
    {
        const auto result = names.emplace(key, candidate);
        auto& selected = result.first->second;
        if (!result.second && candidate.priority < selected.priority) selected = candidate;
    }

public:
    void Add(const TimingRecord& record, const BYTE* base)
    {
        const auto& t = record.timing;
        constexpr UINT HzPerKHz = 1000;
        const auto& origin = record.origin;
        const Candidate candidate{{origin.source, size_t(origin.record - base), origin.choice},
            {t.totalWidth, t.totalHeight, UINT64(t.clockKHz) * HzPerKHz, 0, 1, true}};
        const CtlDisplayMode mode{t.width, t.height, t.refresh, 1};
        const auto signal = candidate.raster.Signal(mode);
        const auto rate = signal.vSyncFreq;
        const Key nominal = ModeKey(mode), physical{t.width, t.height, rate.Numerator, rate.Denominator};
        const auto issue = record.issue == Issue::None && !HasValidFrequencies(signal) ? Issue::WindowsRate : record.issue;
        if (issue != Issue::None) {
            issues[nominal].insert(issue);
            if (rate.Numerator && physical != nominal) issues.emplace(physical, std::set<Issue>{});
            return;
        }
        Register(nominal, candidate);
        if (physical != nominal) Register(physical, candidate);
    }

    const DetailedRaster* Find(CtlDisplayMode mode) const
    {
        if (names.empty() || !Normalize(mode)) return nullptr;
        const auto it = names.find(ModeKey(mode));
        return it == names.end() ? nullptr : &it->second.raster;
    }

    bool HasIssue(CtlDisplayMode mode) const
    {
        return !issues.empty() && Normalize(mode) && issues.count(ModeKey(mode)) != 0;
    }
    const auto& Issues() const { return issues; }
};

inline const BYTE* RangeDescriptor(const BYTE* base)
{
    for (size_t pos = DescriptorOffset; pos < ExtensionCount; pos += DescriptorSize) {
        const BYTE* d = base + pos;
        if (!d[0] && !d[1] && !d[2] && d[3] == RangeLimits) return d;
    }
    return nullptr;
}

inline Formula StandardFormula(const BYTE* base, const BYTE* range)
{
    if (!base || base[19] < 4) return Formula::Gtf;
    if (!range || range[10] == LimitsOnly || range[10] == Cvt) return Formula::Cvt;
    if (range[10] == DefaultGtf || range[10] == SecondaryGtf) return Formula::Gtf;
    return Formula::Unknown;
}

inline UINT CvtSyncLines(const VesaTiming& t)
{
    if (t.height * 4 / 3 == t.width) return 4;
    if (t.height * 16 / 9 == t.width) return 5;
    if (t.height * 16 / 10 == t.width) return 6;
    if (!(t.height % 4) && t.height * 5 / 4 == t.width) return 7;
    if (t.height * 15 / 9 == t.width) return 7;
    return 10;
}

inline double GtfDuty(const BYTE* range, double period)
{
    if (!range || range[10] != SecondaryGtf || 1000.0 / period < 2 * range[12])
        return DefaultDuty - DutySlopePerUs * period;
    const double c = range[13] / 2.0, j = range[17] / 2.0, k = range[16] / 256.0;
    const UINT m = range[14] | (UINT(range[15]) << 8);
    return j + k * (c - j - m * period / 1000.0);
}

// Resolve a nominal refresh rate to a raster using normal-blanking CVT or GTF.
inline Issue FormulaTiming(VesaTiming& t, const BYTE* range, Formula formula)
{
    constexpr UINT GtfPorch = 1, GtfSync = 3, PixelsPerSyncCell = 100; // Eight pixels of horizontal sync per 100 pixels.
    if (formula == Formula::Unknown) return Issue::UnknownFormula;
    const bool cvt = formula == Formula::Cvt;
    const auto rasterIssue = cvt ? Issue::CvtRaster : Issue::GtfRaster;
    const UINT porch = cvt ? CvtPorch : GtfPorch;
    const UINT verticalSync = cvt ? CvtSyncLines(t) : GtfSync;
    const double estimate = (double(UsPerSecond) / t.refresh - MinSyncBackPorchUs) / (t.height + porch);
    const double syncLines = MinSyncBackPorchUs / estimate;
    t.totalHeight = t.height + porch + (cvt ? (std::max)(UINT(syncLines) + 1, verticalSync + CvtBackPorch) :
                                              UINT(std::round(syncLines)));
    const double period = cvt ? estimate : double(UsPerSecond) / (t.refresh * t.totalHeight);
    const double duty = cvt ? (std::max)(20.0, DefaultDuty - DutySlopePerUs * period) : GtfDuty(range, period);
    if (duty < 0 || duty >= 100) return Issue::GtfDuty;
    const double pairs = t.width * duty / ((100 - duty) * CellPair);
    const double width = t.width + CellPair * (cvt ? std::floor(pairs) : std::round(pairs));
    const double clock = width * 1000.0 / period;
    if (width > UINT_MAX / t.totalHeight || clock > UINT_MAX) return rasterIssue;
    t.totalWidth = UINT(width);
    t.clockKHz = cvt ? UINT(clock / ClockStepKHz) * ClockStepKHz : UINT(std::round(clock));
    if (!t.clockKHz) return rasterIssue;
    const UINT horizontalSync = CellPixels * ((t.totalWidth + (cvt ? 0 : PixelsPerSyncCell / 2)) / PixelsPerSyncCell);
    if (!horizontalSync || (t.totalWidth - t.width) / 2 <= horizontalSync || t.totalHeight - t.height <= porch + verticalSync)
        return cvt ? Issue::CvtSync : Issue::GtfSync;
    return Issue::None;
}

inline VesaTiming ReducedCvt(VesaTiming t)
{
    constexpr UINT HorizontalBlank = 160, MinBlankUs = 460, HzPerKHz = 1000;
    const UINT blank = MinBlankUs * t.refresh;
    const UINT lines = blank * t.height / (UsPerSecond - blank) + 1;
    t.totalWidth = t.width + HorizontalBlank;
    t.totalHeight = t.height + (std::max)(lines, CvtPorch + CvtSyncLines(t) + CvtBackPorch);
    const UINT64 hz = UINT64(t.totalWidth) * t.totalHeight * t.refresh;
    t.clockKHz = UINT(hz / (ClockStepKHz * HzPerKHz)) * ClockStepKHz;
    return t;
}

constexpr UINT CvtRates[] = {50, 60, 75, 85};
constexpr BYTE CvtFirstRate = 0x10, CvtReduced60 = 0x01;

inline bool ValidCvtFields(const BYTE* code, UINT preferred)
{
    if (!code[0] || (code[1] & 3) || (code[2] & 0x80)) return false;
    BYTE supported = CvtFirstRate >> preferred;
    if (CvtRates[preferred] == 60) supported |= CvtReduced60;
    return (code[2] & supported) != 0;
}

template<class Add> bool CvtCode(const BYTE* code, Add add)
{
    constexpr UINT horizontal[] = {4, 16, 16, 15}, vertical[] = {3, 9, 10, 9};
    const BYTE flags = code[2];
    const UINT preferred = (flags >> 5) & 3;
    if (!ValidCvtFields(code, preferred)) return false;
    const UINT aspect = (code[1] >> 2) & 3;
    const UINT height = 2 * (1 + (code[0] | UINT(code[1] & 0xf0) << 4));
    const UINT width = height * horizontal[aspect] / vertical[aspect] / CellPixels * CellPixels;
    const bool preferReduced = CvtRates[preferred] == 60 && (flags & CvtReduced60);
    for (UINT i = 0; i < ARRAYSIZE(CvtRates); ++i) {
        if (!(flags & (CvtFirstRate >> i))) continue;
        VesaTiming t{width, height, CvtRates[i]};
        const auto issue = FormulaTiming(t, nullptr, Formula::Cvt);
        add({t, {Source::CvtCode, code, t.refresh == 60 && preferReduced}, issue});
    }
    if (flags & CvtReduced60) {
        const auto rb = ReducedCvt({width, height, 60});
        add({rb, {Source::CvtCode, code, !preferReduced}, width && rb.clockKHz ? Issue::None : Issue::CvtRaster});
    }
    return true;
}

template<class Add> bool ReadCvtCodes(const BYTE* descriptor, Add add)
{
    constexpr size_t first = 6, codeSize = 3;
    for (size_t pos = first; pos < DescriptorSize; pos += codeSize) {
        const BYTE* code = descriptor + pos;
        if (pos != first && !code[0] && !code[1] && !code[2]) continue;
        if (!CvtCode(code, add)) return false;
    }
    return true;
}

template<class Add> void Standard(const BYTE* code, const BYTE* base, Add add)
{
    constexpr UINT UnusedStandardTiming = 0x0101;
    const UINT id = (UINT(code[0]) << 8) | code[1];
    if (!code[0] || id == UnusedStandardTiming) return;
    constexpr UINT horizontal[] = {16, 4, 5, 16}, vertical[] = {10, 3, 4, 9};
    const UINT aspect = code[1] >> 6, width = (code[0] + 31u) * CellPixels;
    const UINT height = !aspect && base && base[19] < 3 ? width : width * vertical[aspect] / horizontal[aspect];
    VesaTiming t{width, height, 60u + (code[1] & 63u)};
    for (const auto& dmt : DmtTimings) {
        const auto& r = dmt.raster;
        if (dmt.standardId == id && r.height == height) {
            add({r, {Source::Standard, code}, Issue::None});
            return;
        }
    }
    if (base && base[19] < 2) return; // No timing formula was defined before EDID 1.2.
    const BYTE* range = base ? RangeDescriptor(base) : nullptr;
    const auto issue = FormulaTiming(t, range, StandardFormula(base, range));
    add({t, {Source::Standard, code}, issue});
}

template<class Add, class ReadDetailed> bool ReadVideoExtension(const BYTE* block, Add add, ReadDetailed readDetailed)
{
    constexpr size_t HeaderSize = 5, CvtSize = 3, StandardSize = 2;
    constexpr BYTE Revision = 1;
    const size_t detailedEnd = HeaderSize + block[2] * DescriptorSize;
    const size_t cvtEnd = detailedEnd + block[3] * CvtSize;
    const size_t end = cvtEnd + block[4] * StandardSize;
    if (block[1] != Revision || end > EdidChecksum) return false;
    for (size_t pos = HeaderSize; pos < detailedEnd; pos += DescriptorSize)
        if (!block[pos] && !block[pos + 1]) return false;
    readDetailed(HeaderSize, detailedEnd);
    for (size_t pos = detailedEnd; pos < cvtEnd; pos += CvtSize)
        if (!CvtCode(block + pos, add)) return false;
    // VTB standard timings use DMT or default GTF independently of the base block's timing formula.
    for (size_t pos = cvtEnd; pos < end; pos += StandardSize)
        Standard(block + pos, nullptr, add);
    return std::all_of(block + end, block + EdidChecksum, [](BYTE value) { return value == 0; });
}

template<class Add> void Established(const BYTE* bits, bool extended, Add add)
{
    // Bit order is MSB first. Non-DMT IBM and Apple identifiers use the last five IDs.
    constexpr BYTE original[] = {0xf0, 0xf1, 4, 0xf2, 5, 6, 8, 9, 10, 11, 0xf3, 0, 16, 17, 18, 36, 0xf4};
    constexpr VesaTiming historical[] = {
        {720, 400, 70, 900, 449, 28320}, {720, 400, 88, 900, 449, 35500},
        {640, 480, 67, 864, 525, 30240}, {832, 624, 75, 1152, 667, 57284},
        {1152, 870, 75, 1456, 915, 100000},
    };
    constexpr BYTE third[] = {
        0x01, 0x02, 0x03, 0x07, 0x0e, 0x0c, 0x13, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x20, 0x21, 0x23, 0x25,
        0x27, 0x2e, 0x2f, 0x30, 0x31, 0x29, 0x2a, 0x2b,
        0x2c, 0x39, 0x3a, 0x3b, 0x3c, 0x33, 0x34, 0x35,
        0x36, 0x37, 0x3e, 0x3f, 0x41, 0x42, 0x44, 0x45, 0x46, 0x47, 0x49, 0x4a,
    };
    const BYTE* ids = extended ? third : original;
    for (size_t bit = 0; bit < (extended ? sizeof(third) : sizeof(original)); ++bit) {
        if (!(bits[bit / 8] & (0x80 >> (bit % 8)))) continue;
        const BYTE id = ids[bit];
        const Origin origin{Source::Established, bits + bit / 8, UINT(bit % 8)};
        if (id >= 0xf0) add({historical[id - 0xf0], origin, Issue::None});
        else {
            const auto dmt = std::find_if(std::begin(DmtTimings), std::end(DmtTimings),
                                         [id](const DmtTiming& d) { return d.id == id; });
            if (dmt != std::end(DmtTimings)) add({dmt->raster, origin, Issue::None});
        }
    }
}

template<class Add> bool Read(const BYTE* base, Add add)
{
    Established(base + EstablishedOffset, false, add);
    for (size_t pos = StandardOffset; pos < DescriptorOffset; pos += 2)
        Standard(base + pos, base, add);
    for (size_t pos = DescriptorOffset; pos < ExtensionCount; pos += DescriptorSize) {
        const BYTE* d = base + pos;
        if (d[0] || d[1] || d[2]) continue;
        if (d[3] == EstablishedIII && d[5] == 0x0a) Established(d + 6, true, add);
        if (base[19] >= 4 && d[3] == ThreeByteCvt && d[5] == 1 && !ReadCvtCodes(d, add)) return false;
        if (d[3] != AdditionalStandard) continue;
        for (size_t i = 5; i < 17; i += 2)
            Standard(d + i, base, add);
    }
    return true;
}
} // namespace base_edid
} // namespace display
