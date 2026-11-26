#include "HdrSample.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace hdr {
namespace {
// D65 linear BT.709 to CIE XYZ, and CIE XYZ to linear BT.2020.
constexpr double ToXyz[3][3] = {
    {0.4123907993, 0.3575843394, 0.1804807884}, {0.2126390059, 0.7151686788, 0.0721923154},
    {0.0193308187, 0.1191947798, 0.9505321522}
};
constexpr double To2020[3][3] = {
    {1.7166511880,-0.3556707838,-0.2533662814}, {-0.6666843518,1.6164812366,0.0157685458},
    {0.0176398574,-0.0427706133,0.9421031212}
};
using Triplet = std::array<double, 3>;

template<class T, class U> Triplet Multiply(const T& matrix, const U& value)
{
    Triplet result{};
    for (size_t row = 0; row < result.size(); ++row)
        for (size_t col = 0; col < result.size(); ++col)
            result[row] += matrix[row][col] * value[col];
    return result;
}

double Pq(double scRgb)
{
    // ST 2084 reference white is 10000 nits. A scRGB value of 1 represents 80 nits.
    const double normalized = std::clamp(scRgb * IDD_SDR_WHITE_NITS / 10000.0, 0.0, 1.0);
    const double power = std::pow(normalized, 2610.0 / 16384);
    return std::pow((3424.0 / 4096 + (2413.0 / 128) * power) / (1 + (2392.0 / 128) * power), 2523.0 / 32);
}

template<class Value> double Lookup(double wire, size_t count, Value value)
{
    const double position = wire * (count - 1);
    const size_t index = static_cast<size_t>(position);
    const size_t next = (std::min)(index + 1, count - 1);
    return value(index) + (position - index) * (value(next) - value(index));
}
} // namespace

bool SameCalibration(const Calibration& a, const Calibration& b)
{
    const auto& x = a.state;
    const auto& y = b.state;
    return SameTarget(a.target, b.target) && SameAdapter(a.renderAdapter, b.renderAdapter) && a.generation == b.generation &&
           x.WireColorSpace == y.WireColorSpace && x.WireRgbBits == y.WireRgbBits &&
           x.SdrWhiteNits == y.SdrWhiteNits && x.HasHdr10Metadata == y.HasHdr10Metadata &&
           (!x.HasHdr10Metadata || !memcmp(x.Hdr10Metadata, y.Hdr10Metadata, sizeof(x.Hdr10Metadata))) &&
           x.GammaType == y.GammaType &&
           (x.GammaType != IDD_GAMMA_TRANSFORM || CtlSameColorTransform(&a.transform, &b.transform)) &&
           (x.GammaType != IDD_GAMMA_RGB256 || !memcmp(a.legacy, b.legacy, sizeof(a.legacy)));
}

Codes Hdr10(const Rgb& scRgb, const Calibration& calibration)
{
    auto xyz = Multiply(ToXyz, scRgb);
    const auto& t = calibration.transform;
    if (t.MatrixEnabled) {
        xyz = Multiply(t.ColorMatrix3x4, xyz);
        for (size_t i = 0; i < xyz.size(); ++i)
            xyz[i] = (xyz[i] + t.ColorMatrix3x4[i][3]) * t.ScalarMultiplier;
    }
    const auto linear = Multiply(To2020, xyz);
    Codes codes{};
    for (size_t i = 0; i < codes.size(); ++i) {
        if (!std::isfinite(linear[i])) throw std::runtime_error("Invalid linear HDR color");
        double wire = Pq(linear[i]);
        if (t.LutEnabled)
            wire = Lookup(wire, ARRAYSIZE(t.LookupTable1D), [&](size_t n) { return t.LookupTable1D[n][i]; });
        else if (calibration.state.GammaType == IDD_GAMMA_RGB256)
            wire = Lookup(wire, ARRAYSIZE(calibration.legacy[i]), [&](size_t n) { return calibration.legacy[i][n] / 65535.0; });
        if (!std::isfinite(wire)) throw std::runtime_error("Invalid HDR gamma table");
        codes[i] = static_cast<UINT>(std::lround(std::clamp(wire, 0.0, 1.0) * 1023));
    }
    return codes;
}
} // namespace hdr
