#pragma once

#include "DisplayProfile.h"
#include <d3d11.h>
#include <algorithm>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

namespace display {
constexpr size_t EdidBlock = 128;
constexpr size_t EdidMajorOffset = 18;
constexpr BYTE SupportedEdidMajor = 1;
constexpr size_t ExtensionCount = 126;
constexpr size_t EdidChecksum = 127;
constexpr BYTE CtaExtension = 0x02;
constexpr size_t CtaHeaderSize = 4;
constexpr BYTE CtaDataBlockRevision = 3;
constexpr BYTE HdmiForumExtensionOverride = 0x78;
constexpr BYTE DisplayIdExtension = 0x70;
constexpr size_t DisplayIdHeader = 5;
constexpr BYTE HdrStaticMetadata = 0x06;
constexpr BYTE PqEotf = 0x04;
constexpr BYTE StaticMetadataType1 = 0x01;

template<class T> inline T Gcd(T a, T b)
{
    while (b) {
        const T next = a % b;
        a = b;
        b = next;
    }
    return a;
}

inline auto ModeKey(const CtlDisplayMode& m) -> std::tuple<UINT, UINT, UINT, UINT>
{
    return std::make_tuple(m.Width, m.Height, m.Numerator, m.Denominator);
}

inline bool Normalize(CtlDisplayMode& m)
{
    if (!m.Width || !m.Height || !m.Numerator || !m.Denominator)
        return false;
    const UINT divisor = Gcd(m.Numerator, m.Denominator);
    m.Numerator /= divisor;
    m.Denominator /= divisor;
    return true;
}

enum class ModeListStatus { Valid, Invalid, TextureTooLarge };

inline ModeListStatus ValidateModes(std::vector<CtlDisplayMode>& modes, bool requireTexture = true)
{
    std::set<decltype(ModeKey(CtlDisplayMode{}))> unique;
    for (auto& mode : modes) {
        if (requireTexture && (mode.Width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                               mode.Height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION))
            return ModeListStatus::TextureTooLarge;
        if (!Normalize(mode) || !unique.insert(ModeKey(mode)).second) return ModeListStatus::Invalid;
    }
    return modes.empty() ? ModeListStatus::Invalid : ModeListStatus::Valid;
}

inline bool KnownDisplayId(const BYTE* block)
{
    const BYTE major = block[1] >> 4;
    return block[0] == DisplayIdExtension && (major == 1 || major == 2);
}

inline bool ValidDisplayIdSection(const BYTE* block)
{
    const size_t checksum = DisplayIdHeader + block[2];
    if (checksum >= EdidChecksum) return false;
    BYTE sum = 0;
    for (size_t i = 1; i <= checksum; ++i) sum += block[i];
    return sum == 0;
}

enum class EdidHeaderStatus { Valid, Invalid, UnsupportedVersion };

inline EdidHeaderStatus CheckEdidHeader(const std::vector<BYTE>& bytes)
{
    constexpr BYTE header[] = {0, 255, 255, 255, 255, 255, 255, 0};
    if (bytes.size() < EdidBlock || !std::equal(std::begin(header), std::end(header), bytes.begin()))
        return EdidHeaderStatus::Invalid;
    return bytes[EdidMajorOffset] == SupportedEdidMajor ? EdidHeaderStatus::Valid : EdidHeaderStatus::UnsupportedVersion;
}

inline const BYTE* HdmiForumOverridePayload(const std::vector<BYTE>& bytes)
{
    constexpr BYTE extendedTagWithTwoBytes = 0xe2;
    constexpr size_t recordSize = 3;
    if (bytes.size() < 2 * EdidBlock || !bytes[ExtensionCount]) return nullptr;
    const BYTE* cta = bytes.data() + EdidBlock;
    if (cta[0] != CtaExtension || cta[1] < CtaDataBlockRevision ||
        cta[2] < CtaHeaderSize + recordSize || cta[2] > EdidChecksum) return nullptr;
    // HDMI 2.1 section 10.3.6 places HF-EEODB first in the first extension.
    const BYTE* record = cta + CtaHeaderSize;
    if (record[0] != extendedTagWithTwoBytes || record[1] != HdmiForumExtensionOverride || record[2] <= 1)
        return nullptr;
    return record + 1;
}

inline bool ValidEdid(const std::vector<BYTE>& bytes)
{
    if (CheckEdidHeader(bytes) != EdidHeaderStatus::Valid) return false;
    const BYTE* overridePayload = HdmiForumOverridePayload(bytes);
    const size_t extensions = overridePayload ? overridePayload[1] : bytes[ExtensionCount];
    if (bytes.size() != EdidBlock * (1 + extensions)) return false;
    for (size_t offset = 0; offset < bytes.size(); offset += EdidBlock) {
        BYTE sum = 0;
        for (size_t i = 0; i < EdidBlock; ++i)
            sum += bytes[offset + i];
        const auto* block = bytes.data() + offset;
        if (sum || (offset && KnownDisplayId(block) && !ValidDisplayIdSection(block))) return false;
    }
    return true;
}

inline bool PayloadFits(const CtlDisplayProfile& header, size_t bytes)
{
    if (bytes < sizeof(header) || header.EdidBytes > bytes - sizeof(header))
        return false;
    const size_t modes = bytes - sizeof(header) - header.EdidBytes;
    return modes % sizeof(CtlDisplayMode) == 0 && modes / sizeof(CtlDisplayMode) == header.ModeCount;
}
} // namespace display
