#pragma once

#include "EdidTiming.h"
#include <cstdlib>

namespace display {
inline DISPLAYCONFIG_RATIONAL SignalRate(UINT64 numerator, UINT64 denominator)
{
    if (!denominator) return {};
    const auto divisor = Gcd(numerator, denominator);
    const auto n = numerator / divisor, d = denominator / divisor;
    if (n > UINT_MAX || d > UINT_MAX) return {};
    return {static_cast<UINT>(n), static_cast<UINT>(d)};
}

inline DISPLAYCONFIG_RATIONAL OtherBroadcastRate(DISPLAYCONFIG_RATIONAL rate)
{
    if (!rate.Denominator) return {};
    const bool integral = rate.Numerator % rate.Denominator == 0;
    const auto nominal = integral ? SignalRate(rate.Numerator, rate.Denominator) :
        SignalRate(UINT64(rate.Numerator) * NtscDivisor, UINT64(rate.Denominator) * NtscScale);
    if (nominal.Denominator != 1 || nominal.Numerator % DualRateMultiple) return {};
    return integral ? SignalRate(UINT64(nominal.Numerator) * NtscScale, NtscDivisor) : nominal;
}

constexpr UINT64 NoClockMatch = (std::numeric_limits<UINT64>::max)();

inline bool HasValidFrequencies(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal)
{
    return signal.pixelRate && signal.hSyncFreq.Numerator && signal.vSyncFreq.Numerator;
}

struct DetailedRaster {
    UINT totalWidth, totalHeight;
    UINT64 clockNumerator;
    UINT clockUnit;
    UINT clockDenominator = 1;
    bool usePhysicalRefresh = false;

    UINT64 PixelRate() const { return (clockNumerator + clockDenominator / 2) / clockDenominator; }
    DISPLAYCONFIG_RATIONAL PhysicalRefresh() const {
        return SignalRate(clockNumerator, UINT64(totalWidth) * totalHeight * clockDenominator);
    }
    // Return the distance in 1/refresh.Denominator Hz, including either half-step boundary.
    UINT64 MatchedClockError(DISPLAYCONFIG_RATIONAL refresh) const {
        const UINT denominator = refresh.Denominator;
        if (!denominator) return NoClockMatch;
        const UINT64 pixels = UINT64(totalWidth) * totalHeight;
        const UINT64 whole = pixels / denominator;
        // Compare before multiplying. Either unreduced clock product can exceed 64 bits.
        if (whole && refresh.Numerator > (clockNumerator + clockUnit) / whole) return NoClockMatch;
        const UINT64 tail = (pixels % denominator) * refresh.Numerator;
        const auto floorHz = whole * refresh.Numerator + tail / denominator;
        const INT64 delta = static_cast<INT64>(clockNumerator) - static_cast<INT64>(floorHz);
        if (std::abs(delta) > clockUnit) return NoClockMatch;
        const UINT64 error = std::abs(delta * denominator - static_cast<INT64>(tail % denominator));
        return error <= UINT64(clockUnit) * denominator / 2 ? error : NoClockMatch;
    }
    bool MatchesNominal(const CtlDisplayMode& mode) const {
        if (!mode.Denominator || mode.Numerator % mode.Denominator) return false;
        const UINT hz = mode.Numerator / mode.Denominator;
        const UINT64 pixels = UINT64(totalWidth) * totalHeight;
        if ((clockNumerator + pixels / 2) / pixels != hz) return false;
        // A detailed 60000/1001 timing names the fractional broadcast mode.
        return MatchedClockError(OtherBroadcastRate({hz, 1})) == NoClockMatch;
    }
    bool SameRaster(const DetailedRaster& other) const {
        return totalWidth == other.totalWidth && totalHeight == other.totalHeight &&
               clockNumerator == other.clockNumerator && clockDenominator == other.clockDenominator;
    }
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO Signal(const CtlDisplayMode& mode) const {
        DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal{};
        signal.activeSize = {mode.Width, mode.Height};
        signal.totalSize = {totalWidth, totalHeight};
        signal.pixelRate = PixelRate();
        signal.hSyncFreq = SignalRate(clockNumerator, UINT64(totalWidth) * clockDenominator);
        const bool nominal = clockUnit && MatchedClockError({mode.Numerator, mode.Denominator}) == NoClockMatch;
        signal.vSyncFreq = (usePhysicalRefresh || nominal) ? PhysicalRefresh() : SignalRate(mode.Numerator, mode.Denominator);
        return signal;
    }
};

struct DetailedTimingRecord {
    DetailedRaster raster;
    bool validSync;
};

struct DetailedTimingMatch {
    const DetailedRaster* raster = nullptr;
    UINT64 clockError = NoClockMatch;
    bool ambiguous = false, invalidFrequency = false, invalidSync = false;

    const DetailedRaster* Selected() const { return ambiguous ? nullptr : raster; }
    void Consider(const DetailedTimingRecord& record, const CtlDisplayMode& mode)
    {
        const auto& candidate = record.raster;
        const auto error = candidate.MatchedClockError({mode.Numerator, mode.Denominator});
        if (error > clockError) return;
        // Outside the clock tolerance, an integer refresh can name only one distinct raster.
        if (error == NoClockMatch && !candidate.MatchesNominal(mode)) return;
        if (!record.validSync) {
            invalidSync = true;
            return;
        }
        if (!HasValidFrequencies(candidate.Signal(mode))) {
            invalidFrequency = true;
            return;
        }
        if (!raster || error < clockError) {
            raster = &candidate;
            clockError = error;
            ambiguous = false;
        } else if (error == NoClockMatch && !raster->SameRaster(candidate)) {
            ambiguous = true;
        }
    }
};

inline DISPLAYCONFIG_VIDEO_SIGNAL_INFO DriverSignal(const CtlDisplayMode& mode, bool includeBlanking)
{
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal{};
    signal.activeSize = signal.totalSize = {mode.Width, mode.Height};
    EdidRaster raster;
    // Driver timings need an exact Windows line rate. Active-only timings can
    // still represent a mode whose line rate with blanking exceeds a DWORD.
    const bool hasRaster = includeBlanking && PreferredRaster(mode, raster) &&
        SignalRate(UINT64(raster.clock) * ClockUnitHz, mode.Width + raster.hBlank).Numerator;
    if (hasRaster &&
        SignalRate(UINT64(mode.Numerator) * (mode.Height + raster.vBlank), mode.Denominator).Numerator) {
        signal.totalSize.cx += raster.hBlank;
        signal.totalSize.cy += raster.vBlank;
    }
    signal.vSyncFreq = SignalRate(mode.Numerator, mode.Denominator);
    signal.hSyncFreq = SignalRate(UINT64(mode.Numerator) * signal.totalSize.cy, mode.Denominator);
    if (!signal.hSyncFreq.Numerator && hasRaster &&
        UINT64(raster.clock) * ClockUnitHz >= UINT64(DisplayIdMaxClock) * TypeVIIClockHz) {
        // Above the Type VII clock limit, publish the rounded scan timing through IddCx.
        const DetailedRaster scan{mode.Width + raster.hBlank, mode.Height + raster.vBlank,
                                  UINT64(raster.clock) * ClockUnitHz, ClockUnitHz};
        return scan.Signal(mode);
    }
    if (signal.hSyncFreq.Denominator)
        signal.pixelRate = UINT64(signal.hSyncFreq.Numerator) * signal.totalSize.cx / signal.hSyncFreq.Denominator;
    return signal;
}

inline auto SignalKey(const DISPLAYCONFIG_VIDEO_SIGNAL_INFO& s)
{
    // For progressive scan the line frequency and totals determine the physical refresh.
    return std::make_tuple(s.activeSize.cx, s.activeSize.cy, s.totalSize.cx, s.totalSize.cy,
                           s.hSyncFreq.Numerator, s.hSyncFreq.Denominator);
}

inline DISPLAYCONFIG_VIDEO_SIGNAL_INFO GeneratedDriverSignal(const CtlDisplayMode& mode, DISPLAYCONFIG_2DREGION total)
{
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal{};
    signal.activeSize = {mode.Width, mode.Height};
    signal.totalSize = UINT64(mode.Numerator) * total.cy <= UINT64(UINT_MAX) * mode.Denominator ? total : signal.activeSize;
    const UINT64 line = UINT64(mode.Numerator) * signal.totalSize.cy;
    signal.hSyncFreq = SignalRate(line, mode.Denominator);
    if (!signal.hSyncFreq.Numerator && line && mode.Denominator) {
        // Windows stores each line-rate field in a DWORD. Retain the requested vertical
        // rate and round the line rate at the finest fixed precision that fits.
        const UINT64 scale = UINT64(UINT_MAX) * mode.Denominator / line;
        if (scale) {
            const UINT64 fraction = ((line % mode.Denominator) * scale + mode.Denominator / 2) / mode.Denominator;
            signal.hSyncFreq = SignalRate((line / mode.Denominator) * scale + fraction, scale);
        }
    }
    signal.vSyncFreq = SignalRate(mode.Numerator, mode.Denominator);
    signal.pixelRate = mode.Denominator ? RoundedEdidClock(mode, signal.totalSize, 1) : 0;
    return signal;
}
} // namespace display
