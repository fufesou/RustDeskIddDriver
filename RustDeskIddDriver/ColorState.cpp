#include "Driver.h"

namespace display {
void ColorOutput::ResetFrames(LUID renderAdapter)
{
    std::lock_guard<std::mutex> guard(lock);
    ++generation;
    renderAdapterLuid = renderAdapter;
    state.FrameAvailable = FALSE;
    state.HasHdr10Metadata = FALSE;
    state.SdrWhiteNits = IDD_SDR_WHITE_NITS;
}

NTSTATUS ColorOutput::Read(OutputBuffer& out, const CtlDisplayTarget* target)
{
    std::lock_guard<std::mutex> guard(lock);
    const size_t header = target ? sizeof(CtlCaptureColor) : sizeof(state);
    const size_t gammaBytes = out.size == header ? 0 : gamma.size();
    if (out.size < header + gammaBytes) return STATUS_BUFFER_TOO_SMALL;
    if (target) {
        memset(out.data, 0, header);
        memcpy(out.data + offsetof(CtlCaptureColor, Generation), &generation, sizeof(generation));
        memcpy(out.data + offsetof(CtlCaptureColor, Target), target, sizeof(*target));
        memcpy(out.data + offsetof(CtlCaptureColor, RenderAdapterLuid), &renderAdapterLuid, sizeof(renderAdapterLuid));
    }
    memcpy(out.data, &state, sizeof(state));
    if (gammaBytes) memcpy(out.data + header, gamma.data(), gammaBytes);
    out.used = header + gammaBytes;
    return STATUS_SUCCESS;
}

#if IDDCX_VERSION_MINOR >= 10
static_assert(sizeof(CtlColorTransform) == sizeof(IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM) &&
              offsetof(CtlColorTransform, LookupTable1D) == offsetof(IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM, LookupTable1D),
              "HDR calibration ABI");
static_assert(IDD_GAMMA_TRANSFORM == IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM &&
              IDD_GAMMA_RGB256 == IDDCX_GAMMARAMP_TYPE_RGB256x3x16 &&
              IDD_WIRE_HDR10 == IDDCX_COLOR_SPACE_G2084_P2020 && IDD_WIRE_10BPC == IDDCX_BITS_PER_COMPONENT_10 &&
              IDD_WIRE_SDR == IDDCX_COLOR_SPACE_G22_P709 && IDD_WIRE_8BPC == IDDCX_BITS_PER_COMPONENT_8 &&
              IDD_WIRE_SDR_WCG == IDDCX_COLOR_SPACE_G22_P709_WCG,
              "HDR wire format ABI");
void ColorOutput::Frame(const IDDCX_METADATA2& frame)
{
    std::lock_guard<std::mutex> guard(lock);
    const auto previous = state;
    state.FrameAvailable = TRUE;
    state.PresentationFrameNumber = frame.PresentationFrameNumber;
    state.ColorSpace = frame.SurfaceColorSpace;
    state.SdrWhiteNits = frame.SdrWhiteLevel;
    FrameMetadata(frame);
    if (!previous.FrameAvailable || previous.ColorSpace != state.ColorSpace || previous.SdrWhiteNits != state.SdrWhiteNits ||
        previous.HasHdr10Metadata != state.HasHdr10Metadata || (state.HasHdr10Metadata &&
        memcmp(previous.Hdr10Metadata, state.Hdr10Metadata, sizeof(state.Hdr10Metadata)))) ++generation;
}

void ColorOutput::FrameMetadata(const IDDCX_METADATA2& frame)
{
    const auto& data = frame.Hdr10FrameMetaData;
    if (!(frame.ValidFlags & IDDCX_METADATA2_VALID_FLAGS_HDR10METADATA)) {
        state.HasHdr10Metadata = FALSE;
        return;
    }
    if (data.Type == IDDCX_HDR10_FRAME_METADATA_TYPE_UNCHANGED) return;
    const bool isDefault = data.Type == IDDCX_HDR10_FRAME_METADATA_TYPE_DEFAULT;
    state.HasHdr10Metadata = isDefault ? hasDefault : data.Type == IDDCX_HDR10_FRAME_METADATA_TYPE_NEW;
    static_assert(sizeof(defaultHdr) == sizeof(state.Hdr10Metadata), "HDR10 ABI");
    if (state.HasHdr10Metadata)
        memcpy(state.Hdr10Metadata, isDefault ? &defaultHdr : &data.NewMetaData, sizeof(defaultHdr));
}

NTSTATUS ColorOutput::SetDefault(const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA& in)
{
    if (in.Type != IDDCX_HDRMETADATA_TYPE_HDR10 || !in.Data.pHdr10 ||
        in.Size < sizeof(IDDCX_HDR10_METADATA)) return STATUS_INVALID_PARAMETER;
    std::lock_guard<std::mutex> guard(lock);
    // Frame() updates the capture generation when a frame actually uses this default.
    defaultHdr = *in.Data.pHdr10;
    hasDefault = true;
    return STATUS_SUCCESS;
}

NTSTATUS ColorOutput::SetGamma(const IDARG_IN_SET_GAMMARAMP& in)
{
    size_t bytes = 0;
    switch (in.Type) {
    case IDDCX_GAMMARAMP_TYPE_DEFAULT: break;
    case IDDCX_GAMMARAMP_TYPE_RGB256x3x16: bytes = sizeof(IDDCX_GAMMARAMP_RGB256x3x16); break;
    case IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM: bytes = sizeof(IDDCX_GAMMARAMP_3X4_COLORSPACE_TRANSFORM); break;
    default: return STATUS_NOT_SUPPORTED;
    }
    if (bytes && (!in.pGammaRampData || in.GammaRampSizeInBytes < bytes)) return STATUS_INVALID_PARAMETER;
    std::lock_guard<std::mutex> guard(lock);
    if (state.GammaType == in.Type && gamma.size() == bytes) {
        const bool same = in.Type == IDDCX_GAMMARAMP_TYPE_3x4_COLORSPACE_TRANSFORM
            ? CtlSameColorTransform(gamma.data(), in.pGammaRampData)
            : !bytes || !memcmp(gamma.data(), in.pGammaRampData, bytes);
        if (same) return STATUS_SUCCESS;
    }
    try {
        gamma.resize(bytes);
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (bytes) memcpy(gamma.data(), in.pGammaRampData, bytes);
    ++generation;
    state.GammaType = in.Type;
    state.GammaBytes = static_cast<UINT>(bytes);
    return STATUS_SUCCESS;
}

void ColorOutput::Commit(const IDDCX_PATH2& path)
{
    const bool active = (path.Flags & IDDCX_PATH_FLAGS_ACTIVE) != 0;
    Commit(active ? path.WireFormatInfo.ColorSpace : IDDCX_COLOR_SPACE_UNINITIALIZED,
           active ? path.WireFormatInfo.BitsPerComponent.Rgb : IDDCX_BITS_PER_COMPONENT_NONE);
}
#endif

void ColorOutput::Commit(UINT space, UINT bits)
{
    std::lock_guard<std::mutex> guard(lock);
    if (space && state.WireColorSpace == space && state.WireRgbBits == bits) return;
    ++generation;
    state.WireColorSpace = space;
    state.WireRgbBits = bits;
    state.FrameAvailable = FALSE;
    state.HasHdr10Metadata = FALSE;
}

} // namespace display
