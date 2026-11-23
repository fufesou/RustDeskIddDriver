#pragma once

#include "CtaVideoTimings.h"
#include "BaseEdidTimings.h"
#include <map>

namespace display {
enum class SignalListStatus { Valid, InvalidFrequency, Duplicate, RequiresYCbCr420, UnusableEdidTiming, InvalidDetailedSync };
struct SignalListResult {
    SignalListStatus status;
    size_t modeIndex;
};
class DescriptorTimings {
public:
    std::vector<BYTE> Generate(const std::vector<CtlDisplayMode>& modes, bool hdr);

    bool Read(const std::vector<BYTE>& edid)
    {
        *this = {};
        if (edid.empty()) return true;
        if (!ValidEdid(edid)) return false;
        const BYTE* overridePayload = HdmiForumOverridePayload(edid);
        const auto add = [&](const base_edid::TimingRecord& record) { standardModes.Add(record, edid.data()); };
        const auto borders = edid[19] < 4 ? DtdBorders::IncludedInBlanking : DtdBorders::Separate;
        ReadDtds(edid.data() + BaseDetailedOffset, ExtensionCount - BaseDetailedOffset, borders);
        for (size_t pos = EdidBlock; pos < edid.size(); pos += EdidBlock) {
            const BYTE* block = edid.data() + pos;
            if (block[0] == CtaExtension && !ReadCta(block, overridePayload)) return false;
            if (KnownDisplayId(block) && !ReadDisplayId(block)) return false;
            if (block[0] == base_edid::VideoTimingExtension && !base_edid::ReadVideoExtension(block, add,
                [&](size_t begin, size_t end) { ReadDtds(block + begin, end - begin, DtdBorders::IncludedInBlanking); })) return false;
        }
        return base_edid::Read(edid.data(), add);
    }

    bool HasHdr10() const { return hdr10; }
    template<class Report> void ForEachTimingIssue(Report report) const
    {
        for (const auto& entry : standardModes.Issues()) {
            const auto& key = entry.first;
            if (entry.second.empty()) continue; // Diagnostics are attached to nominal rates.
            const CtlDisplayMode mode{std::get<0>(key), std::get<1>(key), std::get<2>(key), 1};
            for (const auto issue : entry.second) report(mode, base_edid::TimingIssue(issue));
        }
    }

    const DetailedRaster* Find(const CtlDisplayMode& mode) const { return Match(mode).Selected(); }

    DISPLAYCONFIG_VIDEO_SIGNAL_INFO Signal(const CtlDisplayMode& mode, bool includeBlanking) const
    {
        DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal{};
        Resolve(mode, includeBlanking, signal);
        return signal;
    }

    SignalListResult Validate(const std::vector<CtlDisplayMode>& modes, bool includeBlanking = true) const
    {
        std::set<decltype(SignalKey({}))> unique;
        for (size_t i = 0; i < modes.size(); ++i) {
            DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal{};
            const auto status = Resolve(modes[i], includeBlanking, signal);
            if (status != SignalListStatus::Valid) return {status, i};
            if (!unique.insert(SignalKey(signal)).second) return {SignalListStatus::Duplicate, i};
        }
        return {SignalListStatus::Valid, modes.size()};
    }

private:
    using Entries = std::multimap<std::pair<UINT, UINT>, DetailedTimingRecord>;
    using NamedRasters = std::map<decltype(ModeKey(CtlDisplayMode{})), DetailedRaster>;

    template<class Map> static const typename Map::mapped_type* FindNamed(const Map& rasters, CtlDisplayMode mode)
    {
        if (rasters.empty() || !Normalize(mode)) return nullptr;
        const auto it = rasters.find(ModeKey(mode));
        return it == rasters.end() ? nullptr : &it->second;
    }

    SignalListStatus MissingRgbStatus(const CtlDisplayMode& mode, const DetailedTimingMatch& match) const
    {
        if (!match.raster && match.invalidFrequency) return SignalListStatus::InvalidFrequency;
        if (FindNamed(yCbCr420Modes, mode)) return SignalListStatus::RequiresYCbCr420;
        if (!match.raster && match.invalidSync) return SignalListStatus::InvalidDetailedSync;
        return standardModes.HasIssue(mode) ? SignalListStatus::UnusableEdidTiming : SignalListStatus::Valid;
    }

    SignalListStatus Resolve(const CtlDisplayMode& mode, bool includeBlanking, DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal) const
    {
        if (const auto* prepared = FindNamed(driverModes, mode)) signal = *prepared;
        else {
            const auto match = Match(mode);
            const auto* rgb = match.Selected();
            const auto status = rgb ? SignalListStatus::Valid : MissingRgbStatus(mode, match);
            if (status != SignalListStatus::Valid) return status;
            signal = rgb ? rgb->Signal(mode) : DriverSignal(mode, includeBlanking);
        }
        return HasValidFrequencies(signal) ? SignalListStatus::Valid : SignalListStatus::InvalidFrequency;
    }

    DetailedTimingMatch Match(const CtlDisplayMode& mode) const
    {
        DetailedTimingMatch match;
        if (!mode.Width || !mode.Height || !mode.Numerator || !mode.Denominator) return match;
        const auto range = entries.equal_range({mode.Width, mode.Height});
        for (auto it = range.first; it != range.second; ++it) {
            match.Consider(it->second, mode);
            if (match.clockError == 0) return match;
        }
        // A named rate can describe a different raster within the detailed timing's clock tolerance.
        if (const auto* named = FindNamed(videoModes, mode)) return {named};
        if (const auto* standard = standardModes.Find(mode)) return {standard};
        return match;
    }
    static constexpr size_t BaseDetailedOffset = 54, DtdSize = 18;
    static constexpr BYTE TypeI = 0x03;
    static constexpr size_t DataHeader = 3, DetailedSize = 20;

    static void AddNamedRaster(NamedRasters& rasters, const CtlDisplayMode& name, const DetailedRaster& raster)
    {
        rasters.emplace(ModeKey(name), raster);
        const auto rate = raster.PhysicalRefresh();
        if (rate.Numerator)
            rasters.emplace(ModeKey({name.Width, name.Height, rate.Numerator, rate.Denominator}), raster);
    }

    static DetailedRaster VideoRaster(const CtaVideoTiming& v, bool fractional)
    {
        constexpr UINT HzPerKHz = 1000;
        const bool ntscClock = v.height == 240 || v.height == 480;
        const UINT64 base = UINT64(v.clockKHz) * HzPerKHz;
        const UINT64 integral = ntscClock ? ((UINT64(v.clockKHz) * NtscDivisor + NtscScale / 2) / NtscScale) * HzPerKHz : base;
        if (!fractional) return {v.totalWidth, v.totalHeight, integral, 0, 1, true};
        return {v.totalWidth, v.totalHeight, ntscClock ? base : base * NtscScale, 0, ntscClock ? 1u : NtscDivisor, true};
    }

    static void AddVideoCode(NamedRasters& rasters, const CtaVideoTiming& v)
    {
        AddNamedRaster(rasters, {v.width, v.height, v.refresh, 1}, VideoRaster(v, false));
        // CTA defines both rates for formats whose nominal refresh is a multiple of six.
        if (v.refresh % DualRateMultiple == 0)
            AddNamedRaster(rasters, {v.width, v.height, v.refresh * NtscScale, NtscDivisor}, VideoRaster(v, true));
    }

    bool ReadCta(const BYTE* block, const BYTE* overridePayload = nullptr)
    {
        const size_t start = block[2];
        if (!start) return true;
        if (start < CtaHeaderSize || start > EdidChecksum) return false;
        if (block[1] >= CtaDataBlockRevision &&
            !ReadCtaCollection(block + CtaHeaderSize, start - CtaHeaderSize, overridePayload)) return false;
        ReadDtds(block + start, EdidChecksum - start, DtdBorders::IncludedInBlanking);
        return true;
    }

    bool ReadCtaCollection(const BYTE* data, size_t size, const BYTE* overridePayload = nullptr)
    {
        constexpr BYTE Video = 2, Vendor = 3, Extended = 7, LengthMask = 31;
        for (size_t pos = 0; pos < size;) {
            const BYTE header = data[pos++];
            const size_t length = header & LengthMask;
            if (length > size - pos) return false;
            const BYTE* payload = data + pos;
            if ((header >> 5) == Video) ReadVideoCodes(videoModes, payload, length);
            if ((header >> 5) == Vendor && !ReadHdmiVideo(payload, length)) return false;
            if ((header >> 5) == Extended && length && !ReadCtaExtended(payload, length, overridePayload)) return false;
            pos += length;
        }
        return true;
    }

    bool ReadHdmiVideo(const BYTE* payload, size_t length)
    {
        constexpr BYTE HdmiOui[] = {0x03, 0x0c, 0x00};
        constexpr size_t PhysicalAddressEnd = 5, FlagsOffset = 7, LatencyBytes = 2;
        constexpr BYTE Latency = 0x80, InterlacedLatency = 0x40, VideoPresent = 0x20;
        if (length < sizeof(HdmiOui) || memcmp(payload, HdmiOui, sizeof(HdmiOui))) return true;
        if (length <= FlagsOffset) return length >= PhysicalAddressEnd;
        const BYTE flags = payload[FlagsOffset];
        if ((flags & InterlacedLatency) && !(flags & Latency)) return false;
        size_t pos = FlagsOffset + 1;
        if (flags & Latency) pos += LatencyBytes;
        if (flags & InterlacedLatency) pos += LatencyBytes;
        if (pos > length) return false;
        if (!(flags & VideoPresent)) return true;
        return ReadHdmiVicList(payload + pos, length - pos);
    }

    bool ReadHdmiVicList(const BYTE* video, size_t length)
    {
        constexpr size_t VideoHeaderBytes = 2;
        constexpr BYTE VicLengthShift = 5, StereoLengthMask = 0x1f;
        if (length < VideoHeaderBytes) return false;
        const BYTE lengths = video[VideoHeaderBytes - 1];
        const size_t count = lengths >> VicLengthShift;
        if (count + (lengths & StereoLengthMask) > length - VideoHeaderBytes) return false;
        // HDMI 1.4 VICs 1 through 4 name the same rasters as these CTA VICs.
        constexpr BYTE CtaCodes[] = {0, 95, 94, 93, 98};
        for (size_t i = 0; i < count; ++i) {
            const BYTE vic = video[VideoHeaderBytes + i];
            if (vic && vic < ARRAYSIZE(CtaCodes)) ReadVideoCodes(videoModes, &CtaCodes[vic], 1);
        }
        return true;
    }

    bool ReadCtaExtended(const BYTE* payload, size_t length, const BYTE* overridePayload)
    {
        constexpr BYTE YCbCr420Video = 0x0e;
        switch (payload[0]) {
        case HdmiForumExtensionOverride:
            return payload == overridePayload;
        case YCbCr420Video:
            if (length < 2) return false;
            ReadVideoCodes(yCbCr420Modes, payload + 1, length - 1);
            break;
        case TypeVIITimingTag:
            if (length < 2 || length != 2 + DetailedSize + ((payload[1] >> 4) & 7)) return false;
            ReadDisplayTiming(payload + 2, TypeVIIClockHz);
            break;
        case HdrStaticMetadata:
            if (length >= 3 && (payload[1] & PqEotf) && (payload[2] & StaticMetadataType1)) hdr10 = true;
            break;
        }
        return true;
    }

    static void ReadVideoCodes(NamedRasters& rasters, const BYTE* codes, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            if (const auto* v = CtaShortVideoTiming(codes[i])) AddVideoCode(rasters, *v);
    }

    struct PixelClock { UINT ticks, unit; };
    void AddDetailed(const DetailedAxis& horizontal, const DetailedAxis& vertical, PixelClock clock)
    {
        const DetailedRaster raster{horizontal.active + horizontal.blank, vertical.active + vertical.blank,
                                    UINT64(clock.ticks) * clock.unit, clock.unit};
        const bool valid = horizontal.HasSync() && vertical.HasSync();
        entries.emplace(std::make_pair(horizontal.active, vertical.active), DetailedTimingRecord{raster, valid});
    }

    void ReadDtds(const BYTE* data, size_t bytes, DtdBorders borders)
    {
        // With both stereo bits clear the DTD is monoscopic, regardless of bit 0.
        constexpr BYTE Interlaced = 0x80, StereoMask = 0x60;
        for (size_t pos = 0; pos + DtdSize <= bytes; pos += DtdSize) {
            const BYTE* d = data + pos;
            const UINT clock = EdidWord(d);
            if (!clock || (d[17] & (Interlaced | StereoMask))) continue;
            AddDetailed(ReadDtdAxis(d, true, borders), ReadDtdAxis(d, false, borders), {clock, ClockUnitHz});
        }
    }

    bool ReadDisplayId(const BYTE* block)
    {
        constexpr BYTE CtaDisplayIdTag = 0x81;
        const size_t end = DisplayIdHeader + block[2];
        size_t pos = DisplayIdHeader;
        while (pos + DataHeader <= end) {
            const BYTE* header = block + pos;
            pos += DataHeader;
            if (header[2] > end - pos) return false;
            if (header[0] == CtaDisplayIdTag && !ReadCtaCollection(block + pos, header[2])) return false;
            if ((header[0] == TypeI || header[0] == TypeVIITimingTag) && !ReadDisplayTimings(header, block + pos))
                return false;
            pos += header[2];
        }
        return std::all_of(block + pos, block + end, [](BYTE byte) { return byte == 0; });
    }

    bool ReadDisplayTimings(const BYTE* header, const BYTE* data)
    {
        const UINT unit = header[0] == TypeI ? ClockUnitHz : TypeVIIClockHz;
        const size_t stride = header[0] == TypeI ? DetailedSize : DetailedSize + ((header[1] >> 4) & 7);
        if (!header[2] || header[2] % stride) return false;
        for (size_t pos = 0; pos < header[2]; pos += stride) ReadDisplayTiming(data + pos, unit);
        return true;
    }

    void ReadDisplayTiming(const BYTE* d, UINT unit)
    {
        constexpr BYTE Interlaced = 0x10, StereoMask = 0x60, MonoOrStereo = 0x40;
        const BYTE stereo = d[3] & StereoMask;
        if ((d[3] & Interlaced) || (stereo && stereo != MonoOrStereo)) return;
        const UINT clock = 1 + (EdidWord(d) | UINT(d[2]) << 16);
        AddDetailed(ReadDisplayIdAxis(d + 4), ReadDisplayIdAxis(d + 12), {clock, unit});
    }

    Entries entries;
    NamedRasters videoModes, yCbCr420Modes;
    base_edid::NamedTimings standardModes;
    std::map<decltype(ModeKey(CtlDisplayMode{})), DISPLAYCONFIG_VIDEO_SIGNAL_INFO> driverModes;
    bool hdr10 = false;
};
} // namespace display
