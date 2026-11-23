#include "HdrSample.h"
#include <cstring>
#include <stdexcept>

namespace hdr {
namespace {
using Bytes = std::vector<BYTE>;
constexpr size_t SignatureSize = 8;
constexpr size_t ChunkOverhead = 12;

void BigEndian(Bytes& bytes, UINT32 value, unsigned length)
{
    for (unsigned i = length; i != 0; --i) bytes.push_back(static_cast<BYTE>(value >> (8 * (i - 1))));
}

void Chunk(Bytes& output, const char (&type)[5], const Bytes& data)
{
    BigEndian(output, static_cast<UINT32>(data.size()), 4);
    const size_t start = output.size();
    output.insert(output.end(), type, type + 4);
    output.insert(output.end(), data.begin(), data.end());
    UINT32 crc = 0xffffffff;
    for (size_t i = start; i < output.size(); ++i) {
        crc ^= output[i];
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    BigEndian(output, ~crc, 4);
}

Bytes ColorChunks(const CtlColorState& state)
{
    Bytes chunks;
    Chunk(chunks, "cICP", {9, 16, 0, 1}); // BT.2020 primaries, PQ, identity matrix, full range.
    if (!state.HasHdr10Metadata) return chunks;
    const auto* hdr = state.Hdr10Metadata;
    Bytes mastering;
    for (unsigned i = 0; i < 8; ++i) BigEndian(mastering, hdr[i], 2);
    // PNG luminances are expressed in units of 0.0001 cd/m2.
    constexpr UINT32 UnitsPerNit = 10000;
    BigEndian(mastering, hdr[8] * UnitsPerNit, 4);
    BigEndian(mastering, hdr[9], 4);
    Chunk(chunks, "mDCV", mastering);
    Bytes light;
    BigEndian(light, hdr[10] * UnitsPerNit, 4);
    BigEndian(light, hdr[11] * UnitsPerNit, 4);
    Chunk(chunks, "cLLI", light);
    return chunks;
}

bool ColorTag(const BYTE* tag)
{
    for (auto name : {"sRGB", "iCCP", "gAMA", "cHRM", "cICP", "mDCV", "cLLI"})
        if (memcmp(name, tag, 4) == 0) return true;
    return false;
}
} // namespace

std::vector<BYTE> TagPng(const std::vector<BYTE>& png, const CtlColorState& state)
{
    const auto color = ColorChunks(state);
    if (png.size() < SignatureSize + ChunkOverhead || memcmp(png.data() + SignatureSize + 4, "IHDR", 4))
        throw std::runtime_error("Invalid Windows PNG header");
    Bytes output;
    output.reserve(png.size() + color.size());
    output.insert(output.end(), png.begin(), png.begin() + SignatureSize);
    for (size_t pos = SignatureSize; pos < png.size();) {
        if (png.size() - pos < ChunkOverhead) throw std::runtime_error("Incomplete Windows PNG chunk");
        const auto* p = png.data() + pos;
        const size_t count = (UINT32(p[0]) << 24) | (UINT32(p[1]) << 16) | (UINT32(p[2]) << 8) | p[3];
        if (count > png.size() - pos - ChunkOverhead) throw std::runtime_error("Invalid Windows PNG chunk length");
        const auto end = pos + count + ChunkOverhead;
        if (!ColorTag(p + 4)) output.insert(output.end(), png.begin() + pos, png.begin() + end);
        if (!memcmp(p + 4, "IHDR", 4)) output.insert(output.end(), color.begin(), color.end());
        pos = end;
    }
    return output;
}
} // namespace hdr
