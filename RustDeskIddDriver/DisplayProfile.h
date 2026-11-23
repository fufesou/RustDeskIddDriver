#pragma once

#include <windows.h>
#include <winioctl.h>
#include <stddef.h>
#include <string.h>

// All requests use fixed-width fields and METHOD_BUFFERED for 32/64-bit clients.
#define IDD_PROFILE_IOCTL(function) CTL_CODE(FILE_DEVICE_UNKNOWN, function, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_IDD_APPLY_PROFILE IDD_PROFILE_IOCTL(0x800)
#define IOCTL_IDD_SET_RENDER_ADAPTER IDD_PROFILE_IOCTL(0x801)
#define IOCTL_IDD_QUERY_CURSOR IDD_PROFILE_IOCTL(0x802)
#define IOCTL_IDD_QUERY_COLOR IDD_PROFILE_IOCTL(0x803)
// CtlDisplayProfile with ENABLED, mode entries and no EDID. Updates the named active monitor.
#define IOCTL_IDD_UPDATE_PROFILE_MODES IDD_PROFILE_IOCTL(0x804)
// CtlSessionProfile wraps a regular profile packet with a unique ID for this attachment.
// An enabled session profile creates on an empty connector. Disabling or updating requires that ID.
#define IOCTL_IDD_SESSION_PROFILE IDD_PROFILE_IOCTL(0x805)
#define IOCTL_IDD_SESSION_MODES IDD_PROFILE_IOCTL(0x806)
// UINT connector in, CtlConnectorState out (or just the GUID for a GUID-sized buffer).
// An empty connector returns STATUS_NOT_FOUND.
#define IOCTL_IDD_QUERY_CONNECTOR IDD_PROFILE_IOCTL(0x807)
// CtlProfileDisconnect. A stale observation returns STATUS_RETRY without detaching a monitor.
#define IOCTL_IDD_DISCONNECT_PROFILE IDD_PROFILE_IOCTL(0x808)

#define IDD_PROFILE_VERSION 1u
#define IDD_MAX_CONNECTORS 10u
#define IDD_PROFILE_ENABLED 1u
#define IDD_PROFILE_HDR 2u
#define IDD_PROFILE_CURSOR 4u
#define IDD_CURSOR_EXTENT 128u
#define IDD_CURSOR_BYTES (IDD_CURSOR_EXTENT * IDD_CURSOR_EXTENT * 4u)
#define IDD_SDR_WHITE_NITS 80u

typedef struct {
    UINT Width;
    UINT Height;
    UINT Numerator;
    UINT Denominator;
} CtlDisplayMode;

// Followed by ModeCount CtlDisplayMode entries and EdidBytes of optional EDID.
// Mode zero is preferred. Reapplying the same profile is idempotent.
typedef struct {
    UINT Version;
    UINT ConnectorIndex;
    GUID ContainerId;
    UINT Flags;
    UINT ModeCount;
    UINT EdidBytes;
} CtlDisplayProfile;

typedef struct {
    GUID AttachmentId;
    CtlDisplayProfile Profile;
} CtlSessionProfile;

typedef struct {
    UINT ConnectorIndex;
    UINT LastShapeId; // Zero requests the current shape.
} CtlCursorQuery;

// Pixels follow this header when ShapeUpdated is true. CursorType uses
// IDDCX_CURSOR_SHAPE_TYPE. SDR white is expressed in nits.
typedef struct {
    BOOL Visible;
    BOOL PositionValid;
    UINT PositionId;
    INT X;
    INT Y;
    UINT SdrWhiteNits;
    BOOL ShapeUpdated;
    UINT ShapeId;
    UINT CursorType;
    UINT Width;
    UINT Height;
    UINT Pitch;
    UINT HotX;
    UINT HotY;
} CtlCursorState;

// ColorSpace is DXGI_COLOR_SPACE_TYPE. HDR10 metadata has the WDK
// IDDCX_HDR10_METADATA layout. Optional gamma bytes follow this header
// in the WDK layout identified by GammaType. A header-sized query returns
// the current GammaBytes so clients can allocate a full response.
typedef struct {
    BOOL FrameAvailable;
    UINT PresentationFrameNumber;
    UINT WireColorSpace; // IDDCX_COLOR_SPACE of the committed path.
    UINT WireRgbBits;    // IDDCX_BITS_PER_COMPONENT mask.
    UINT ColorSpace;
    UINT SdrWhiteNits;
    BOOL HasHdr10Metadata;
    USHORT Hdr10Metadata[12];
    UINT GammaType;
    UINT GammaBytes;
} CtlColorState;

typedef struct {
    UINT ConnectorIndex;
    GUID ContainerId;
} CtlMonitorIdentity;
typedef CtlMonitorIdentity CtlColorQuery;

typedef struct {
    GUID ContainerId;
    UINT64 Arrival; // Changes on each successful arrival on this device and connector.
} CtlConnectorState;

typedef struct {
    CtlMonitorIdentity Monitor;
    UINT64 Arrival; // From QUERY_CONNECTOR on this handle, or zero to confirm an empty slot.
} CtlProfileDisconnect;

// Optional named forms of QUERY_CURSOR and SET_RENDER_ADAPTER. The driver checks the identity
// under the same lock as the operation so a replacement monitor cannot receive the request.
typedef struct {
    CtlMonitorIdentity Monitor;
    UINT LastShapeId;
} CtlNamedCursorQuery;

typedef struct {
    CtlMonitorIdentity Monitor;
    LUID RenderAdapter;
} CtlNamedRenderAdapter;

// Windows assigns this adapter LUID and target ID when the monitor arrives.
typedef struct {
    LUID AdapterLuid;
    UINT TargetId;
} CtlDisplayTarget;

// A named color query returns this header followed by the optional gamma bytes.
typedef struct {
    CtlColorState Color;
    UINT64 Generation; // Changes when the effective color state or swap chain changes.
    CtlDisplayTarget Target;
    LUID RenderAdapterLuid; // Render adapter of the swap chain that supplies the available frame.
} CtlCaptureColor;

// SDK-only consumers use these layouts to read the WDK gamma payload.
#define IDD_GAMMA_RGB256 2u
#define IDD_GAMMA_TRANSFORM 3u
#define IDD_WIRE_SDR 1u
#define IDD_WIRE_HDR10 2u
#define IDD_WIRE_SDR_WCG 3u
#define IDD_WIRE_8BPC 2u
#define IDD_WIRE_10BPC 4u
typedef struct {
    BOOL MatrixEnabled;
    float ColorMatrix3x4[3][4];
    float ScalarMultiplier;
    BOOL LutEnabled;
    float LookupTable1D[4096][3];
} CtlColorTransform;

#ifdef __cplusplus
inline bool CtlSameColorValues(const BYTE* a, const BYTE* b, size_t bytes)
{
    if (memcmp(a, b, bytes) == 0) return true;
    // Different encodings of zero represent the same calibration value.
    for (size_t pos = 0; pos < bytes; pos += sizeof(float)) {
        float x, y;
        memcpy(&x, a + pos, sizeof(x));
        memcpy(&y, b + pos, sizeof(y));
        if (x != y) return false;
    }
    return true;
}

inline bool CtlSameColorTransform(const void* first, const void* second)
{
    const auto* a = static_cast<const BYTE*>(first);
    const auto* b = static_cast<const BYTE*>(second);
    // IddCx supplies a byte buffer. Only the fields of an enabled stage are valid.
    const auto sameStage = [&](size_t flag, size_t begin, size_t end) {
        BOOL enabledA, enabledB;
        memcpy(&enabledA, a + flag, sizeof(enabledA));
        memcpy(&enabledB, b + flag, sizeof(enabledB));
        if (!enabledA || !enabledB) return !enabledA && !enabledB;
        return CtlSameColorValues(a + begin, b + begin, end - begin);
    };
    constexpr size_t lutFlag = offsetof(CtlColorTransform, LutEnabled);
    return sameStage(offsetof(CtlColorTransform, MatrixEnabled), offsetof(CtlColorTransform, ColorMatrix3x4), lutFlag) &&
           sameStage(lutFlag, offsetof(CtlColorTransform, LookupTable1D), sizeof(CtlColorTransform));
}
#endif
