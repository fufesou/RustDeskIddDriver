// Offline driver contracts. Build with the Windows SDK and WDK via run-driver-tests.ps1.
#include "../RustDeskIddDriver/Driver.h"
#include "../RustDeskIddApp/HdrSample.h"
#include "../RustDeskIddApp/ProfileFile.h"
#include <cassert>
#include <cstdio>

using namespace display;
void CheckDriverDevice();

static const GUID ScreenId = {0xc26c0a51, 0xa356, 0x4d96, {0xac, 0xd5, 0xdd, 0x62, 0x4a, 0x0a, 0x2b, 0xe9}};

static std::vector<BYTE> Packet(UINT flags, const Modes& modes, const std::vector<BYTE>& edid = {}) {
    return profiles::Packet({{IDD_PROFILE_VERSION, 0, ScreenId, flags}, modes, edid});
}

static NTSTATUS Decode(const std::vector<BYTE>& data, Profile& out) {
    return DecodeProfile(*reinterpret_cast<const CtlDisplayProfile*>(data.data()), data.size(), out);
}

static void CheckProfiles() {
    const Modes modes{{3840, 2160, 120, 1}, {1920, 1080, 120000, 2002}};
    auto data = Packet(IDD_PROFILE_ENABLED | IDD_PROFILE_HDR, modes);
    Profile first, second;
    assert(NT_SUCCESS(Decode(data, first)) && ValidEdid(first.edid) && first.timings.HasHdr10());
    assert(SameModes(first.modes, {{3840, 2160, 120, 1}, {1920, 1080, 60000, 1001}}));
    assert(NT_SUCCESS(Decode(data, second)) && SameProfile(first, second));
    second.modes.pop_back();
    assert(!SameProfile(first, second));
    data.pop_back();
    assert(Decode(data, second) == STATUS_INVALID_PARAMETER);
    assert(Decode(Packet(IDD_PROFILE_ENABLED, {{1, 1, 1, 0}}), second) == STATUS_INVALID_PARAMETER);
    assert(Decode(Packet(IDD_PROFILE_ENABLED, {{1, 1, 1, 1}, {1, 1, 2, 2}}), second) == STATUS_INVALID_PARAMETER);
    assert(NT_SUCCESS(Decode(Packet(0, {}), second)));
}

static void CheckTimingAndLegacyModes() {
    constexpr DWORD prefix = offsetof(CtlMonitorModes, Modes);
    constexpr DWORD item = sizeof(CtlMonitorModes{}.Modes[0]);
    constexpr UINT maxModes = (MAXDWORD - prefix) / item;
    assert(!CtlModeRequestBytes(0) && !CtlModeRequestBytes(maxModes + 1));
    assert(CtlModeRequestBytes(maxModes) == prefix + maxModes * item);
    const CtlDisplayMode mode{1920, 1080, 60000, 1001};
    const Profile noDescriptor;
    const auto monitor = Timing(mode, true, noDescriptor);
    const auto target = Timing(mode, false, noDescriptor);
    assert(monitor.vSyncFreq.Numerator == 60000 && monitor.vSyncFreq.Denominator == 1001);
    assert(monitor.hSyncFreq.Numerator == 64800000 && monitor.hSyncFreq.Denominator == 1001);
    assert(target.pixelRate == UINT64(1920) * 1080 * 60000 / 1001);
    assert(monitor.AdditionalSignalInfo.vSyncFreqDivider == 0 && target.AdditionalSignalInfo.vSyncFreqDivider == 1);
    auto dell = LegacyProfile({0, MONITOR_EDID_MOD_DELL_S2719DGF, ScreenId});
    assert(ValidEdid(dell->edid) && ModeKey(dell->modes.front()) == ModeKey({2560,1440,144,1}));
    Profile imported;
    const CtlDisplayMode exactDell{2560, 1440, 3637500, 25177};
    assert(NT_SUCCESS(Decode(Packet(IDD_PROFILE_ENABLED, {exactDell}, dell->edid), imported)));
    assert(Decode(Packet(IDD_PROFILE_ENABLED, {dell->modes.front(), exactDell}, dell->edid), imported) == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET);
}

static void CheckRenderLimits() {
    std::array<AdapterMeasurement, 5> gpus{{{S_OK,DXGI_ERROR_DEVICE_REMOVED}, {S_OK,S_FALSE,8192},
        {S_OK,S_OK,4096}, {S_OK,DXGI_ERROR_DEVICE_RESET}, {DXGI_ERROR_NOT_FOUND}}};
    const auto next = [&](UINT index) { return gpus.at(index); };
    UINT limit{};
    assert(CommonRenderLimit(next, limit) == S_OK && limit == 4096);
    gpus[1].status = gpus[2].status = DXGI_ERROR_UNSUPPORTED;
    assert(CommonRenderLimit(next, limit) == DXGI_ERROR_DEVICE_REMOVED);
}

static void CheckSharedEdid(const Modes& modes, NTSTATUS expected, UINT flags = IDD_PROFILE_ENABLED) {
    auto generated = std::make_shared<Profile>(), imported = std::make_shared<Profile>();
    assert(NT_SUCCESS(Decode(Packet(flags, modes), *generated)));
    assert(NT_SUCCESS(Decode(Packet(flags, modes, generated->edid), *imported)));
    ++imported->header.ContainerId.Data1;
    imported->header.ConnectorIndex = 1;
    for (const auto& first : {generated, imported}) {
        DescriptorDirectory directory;
        assert(NT_SUCCESS(directory.Add(first)));
        assert(directory.Add(first == generated ? imported : generated) == expected);
    }
}

static void CheckPreferredEdid() {
    CheckSharedEdid({{1920,1080,60,1}}, STATUS_SUCCESS);
    CheckSharedEdid({{1920,1080,60,1}, {2560,1440,144,1}}, STATUS_OBJECT_NAME_COLLISION);
    CheckSharedEdid({{640,480,2997,50}, {640,480,60000,1001}}, STATUS_SUCCESS);
    CheckSharedEdid({{1920,1080,60,1}, {640,480,60,1}, {640,480,60000,1001}}, STATUS_SUCCESS, IDD_PROFILE_ENABLED | IDD_PROFILE_HDR);
    CheckSharedEdid({{3840,2160,120,1}, {1920,1080,60,1}, {640,480,60,1}, {640,480,60000,1001}}, STATUS_SUCCESS);
    Profile generated, imported;
    const CtlDisplayMode fractional{1920, 1080, 60000, 1001};
    assert(NT_SUCCESS(Decode(Packet(IDD_PROFILE_ENABLED, {fractional}), generated)));
    const BYTE* dtd = generated.edid.data() + 54;
    const UINT width = dtd[2] | ((dtd[4] & 0xf0) << 4);
    const UINT height = dtd[5] | ((dtd[7] & 0xf0) << 4);
    assert(width == fractional.Width && height == fractional.Height);
    const UINT htotal = width + dtd[3] + ((dtd[4] & 15) << 8);
    const UINT vtotal = height + dtd[6] + ((dtd[7] & 15) << 8);
    const auto signal = Timing(fractional, true, generated);
    assert(signal.totalSize.cx == htotal && signal.totalSize.cy == vtotal);
    assert(NT_SUCCESS(Decode(Packet(IDD_PROFILE_ENABLED, {fractional}, generated.edid), imported)));
    const auto importedSignal = Timing(fractional, true, imported);
    assert(importedSignal.totalSize.cx == htotal && importedSignal.totalSize.cy == vtotal);
    assert(importedSignal.pixelRate == UINT64(EdidWord(dtd)) * 10000 && importedSignal.pixelRate == signal.pixelRate);
    assert(importedSignal.pixelRate * importedSignal.hSyncFreq.Denominator ==
           UINT64(importedSignal.hSyncFreq.Numerator) * importedSignal.totalSize.cx);
    assert(signal.pixelRate * fractional.Denominator == UINT64(htotal) * vtotal * fractional.Numerator);
    const auto firstEdid = generated.edid;
    const CtlDisplayMode high{3840, 2160, 120, 1};
    assert(NT_SUCCESS(Decode(Packet(IDD_PROFILE_ENABLED | IDD_PROFILE_HDR, {high}), generated)));
    assert(ValidEdid(generated.edid) && generated.timings.HasHdr10() && generated.edid != firstEdid);
    const BYTE* cta = generated.edid.data() + EdidBlock;
    assert(cta[0] == CtaExtension && cta[4] == 0x42 && cta[5] == 118 && cta[6] == 1);
    assert(NT_SUCCESS(Decode(Packet(IDD_PROFILE_ENABLED, {high}, firstEdid), imported)));
    assert(imported.edid == firstEdid && !imported.generated);
    for (const auto& mode : Modes{{5120,1440,240,1}, {320,240,1001,1000}, {160,90,1,10}, {1,1,1,1}}) {
        assert(NT_SUCCESS(Decode(Packet(IDD_PROFILE_ENABLED, {mode}), generated)));
        assert(ValidEdid(generated.edid) && generated.edid[ExtensionCount] == (mode.Width > DtdMaxActive));
        const auto signal = Timing(mode, false, generated);
        assert(signal.vSyncFreq.Numerator == mode.Numerator && signal.vSyncFreq.Denominator == mode.Denominator);
        assert(signal.pixelRate * mode.Denominator == UINT64(signal.totalSize.cx) * signal.totalSize.cy * mode.Numerator);
        assert(EdidWord(generated.edid.data() + 54) >= 1000 && bool(generated.timings.Find(mode)) == (mode.Width != 160));
        assert(NT_SUCCESS(Decode(Packet(IDD_PROFILE_ENABLED, {mode}, generated.edid), imported)));
        if (generated.timings.Find(mode)) assert(imported.timings.Find(mode) && Timing(mode, false, imported).pixelRate == signal.pixelRate);
        if (mode.Width == 5120) assert(signal.totalSize.cx == 5200 && signal.totalSize.cy == 1460);
    }
}

static void CheckVideoTimingExtension() {
    Profile p;
    p.edid = p.timings.Generate({{1280,720,60,1}}, false);
    p.edid[ExtensionCount] = 2;
    p.edid.resize(3 * EdidBlock);
    BYTE* vtb = p.edid.data() + EdidBlock;
    // 1080p75 DTD with zero horizontal front porch, 4K and 1080p60 reduced-blanking CVT, and 1280x1024 DMT.
    const BYTE records[] = {0x10,1,1,2,1, 0x30,0x48,0x80,0x18,0x71,0x38,0x28,0x40,
        0,44,0x45,0,0,0,0,0,0,0x1e, 0x37,0x44,0x29, 0x1b,0x24,0x21, 0x81,0x80};
    std::copy(std::begin(records), std::end(records), vtb);
    BYTE* cta = vtb + EdidBlock; // The CTA 1080p75 DTD has a negative horizontal back porch.
    const BYTE ctaRecords[] = {CtaExtension,3,6,0, 0x41,16, 0x80,0x49,0x80,0x40,0x71,0x38,0x28,0x40,
        44,44,0x45,0x40,0,0,0,0,0,0x1e};
    std::copy(std::begin(ctaRecords), std::end(ctaRecords), cta);
    for (auto block : {p.edid.data(), vtb, cta}) FixChecksum(block);
    for (unsigned order = 0; order < 2; ++order) {
        assert(p.timings.Read(p.edid));
        const auto timing = Timing({1920,1080,75,1}, false, p);
        assert(timing.pixelRate == 184800000 && timing.totalSize.cx == 2200 && timing.totalSize.cy == 1120);
        assert(Timing({3840,2160,60,1}, false, p).pixelRate == 533250000);
        assert(Timing({1280,1024,60,1}, false, p).pixelRate == 108000000);
        assert(Timing({1920,1080,60,1}, false, p).pixelRate == 148500000);
        std::swap_ranges(vtb, cta, cta);
    }
    for (size_t field : {EdidMajorOffset, EdidBlock + 2, EdidBlock + 3, EdidBlock + 4}) {
        auto invalid = p.edid;
        invalid[field] = 255;
        FixChecksum(invalid.data() + field / EdidBlock * EdidBlock);
        assert(!p.timings.Read(invalid));
    }
}

static void CheckCursorSetup()
{
    NTSTATUS cursor = STATUS_DEVICE_NOT_READY;
    bool videoStarted = false;
    const auto start = [&] { videoStarted = true; return S_OK; };
    assert(SUCCEEDED(StartMonitorPath(start, [] { return STATUS_NOT_SUPPORTED; }, cursor)));
    assert(videoStarted && cursor == STATUS_NOT_SUPPORTED);
    assert(SUCCEEDED(StartMonitorPath(start, []() -> NTSTATUS { throw std::bad_alloc(); }, cursor)));
    assert(cursor == STATUS_INSUFFICIENT_RESOURCES);
    bool configured = false;
    const auto enable = [&] { configured = true; return STATUS_SUCCESS; };
    assert(FAILED(StartMonitorPath([] { return E_FAIL; }, enable, cursor)) && !configured);
    assert(SUCCEEDED(StartMonitorPath(start, enable, cursor)) && configured && NT_SUCCESS(cursor));
}

static void CheckCursor()
{
    CursorSnapshot cursor;
    cursor.Prepare();
    CursorUpdate update;
    auto input = cursor.Input();
    assert(input.LastShapeId == 0 && input.ShapeBufferSizeInBytes == IDD_CURSOR_BYTES);
    const BYTE shape[] = {0x10, 0x20, 0x30, 0xff};
    memcpy(input.pShapeBuffer, shape, sizeof(shape));
    update.basic.IsCursorVisible = update.basic.IsCursorShapeUpdated = TRUE;
    auto& info = update.basic.CursorShapeInfo;
    info.ShapeId = 3;
    info.Width = info.Height = 1;
    info.Pitch = sizeof(shape);
    info.CursorType = IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR;
    update.basic.X = -4;
    update.basic.Y = 7;
    update.positionId = 9;
    update.white = 203;
    assert(NT_SUCCESS(cursor.Accept(update)));
    std::vector<BYTE> bytes(sizeof(CtlCursorState) + sizeof(shape));
    OutputBuffer output{bytes.data(), bytes.size()};
    assert(NT_SUCCESS(cursor.Write({0, 0}, output)) && output.used == bytes.size());
    CtlCursorState header{};
    memcpy(&header, bytes.data(), sizeof(header));
    assert(header.Visible && header.PositionValid && header.PositionId == 9 && header.X == -4 && header.Y == 7);
    assert(header.ShapeUpdated && header.ShapeId == 3 && header.SdrWhiteNits == 203);
    assert(!memcmp(bytes.data() + sizeof(header), shape, sizeof(shape)));
    assert(NT_SUCCESS(cursor.Write({0, 3}, output)) && output.used == sizeof(header));
    output.size = sizeof(header);
    assert(cursor.Write({0, 0}, output) == STATUS_BUFFER_TOO_SMALL);
    update.basic.IsCursorVisible = update.basic.IsCursorShapeUpdated = FALSE;
    assert(NT_SUCCESS(cursor.Accept(update)) && NT_SUCCESS(cursor.Write({0, 0}, output)));
    memcpy(&header, bytes.data(), sizeof(header));
    assert(!header.Visible && !header.ShapeUpdated && !header.PositionValid);
    update.basic.IsCursorVisible = update.basic.IsCursorShapeUpdated = TRUE;
    info.Pitch = 1;
    assert(cursor.Accept(update) == STATUS_DATA_ERROR);
    cursor.Reset();
    assert(cursor.Input().LastShapeId == 0);
}

static void CheckHdrOutput() {
    const auto calibration = std::make_unique<hdr::Calibration>();
    calibration->state.GammaType = IDD_GAMMA_TRANSFORM;
    const auto changed = std::make_unique<hdr::Calibration>(*calibration);
    changed->transform.LookupTable1D[0][0] = 1;
    for (const auto& [white, code] : {std::pair<float, UINT>{0.0f, 0u}, {1.0f, 497u}, {12.5f, 769u}})
        assert((hdr::Hdr10({white, white, white}, *changed) == hdr::Codes{code, code, code}));
    calibration->transform.MatrixEnabled = changed->transform.MatrixEnabled = TRUE;
    changed->transform.ScalarMultiplier = -0.0f;
    assert(hdr::SameCalibration(*calibration, *changed));
}

#if IDDCX_VERSION_MINOR >= 10
static void CheckVersionGate() {
    size_t sizes[IDD_STRUCTURE_TABLE_NUM_ENTRIES]{};
    IddStructures = sizes;
    IddClientVersionHigherThanFramework = TRUE;
    IddFunctionCount = IddCxAdapterDisplayConfigUpdateTableIndex + 1;
    IddStructureCount = INDEX_IDDCX_DISPLAYCONFIGPATH + 1;
    sizes[INDEX_IDD_CX_CLIENT_CONFIG] = offsetof(IDD_CX_CLIENT_CONFIG, EvtIddCxParseMonitorDescription2);
    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    assert(config.Size == sizes[INDEX_IDD_CX_CLIENT_CONFIG] && config.Size < sizeof(config));
    assert(IDD_IS_FUNCTION_AVAILABLE(IddCxMonitorSetupHardwareCursor) && !AdvancedColorAvailable());
    CheckDriverDevice();
    IddFunctionCount = IddFunctionTableNumEntries;
    assert(!AdvancedColorAvailable()); // The old callback structure still excludes advanced color.
    sizes[INDEX_IDD_CX_CLIENT_CONFIG] = sizeof(config);
    assert(AdvancedColorAvailable());
    IddClientVersionHigherThanFramework = FALSE;
    IddStructures = nullptr;
}

static void CheckHdrMetadata() {
    ColorOutput color;
    const CtlDisplayTarget target{};
    const auto read = [&] {
        CtlCaptureColor capture{};
        OutputBuffer buffer{reinterpret_cast<BYTE*>(&capture), sizeof(capture)};
        assert(NT_SUCCESS(color.Read(buffer, &target)) && buffer.used == sizeof(capture));
        return capture;
    };
    const IDDCX_PATH2 path{sizeof(IDDCX_PATH2), nullptr, IDDCX_PATH_FLAGS_ACTIVE, {},
                           {IDDCX_COLOR_SPACE_G2084_P2020, {IDDCX_BITS_PER_COMPONENT_10}}};
    color.Commit(path);
    IDDCX_HDR10_METADATA hdr{};
    hdr.MaxContentLightLevel = 1000;
    IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA input{IDDCX_HDRMETADATA_TYPE_HDR10, sizeof(hdr), {&hdr}};
    assert(NT_SUCCESS(color.SetDefault(input)));
    IDDCX_METADATA2 frame{};
    frame.ValidFlags = IDDCX_METADATA2_VALID_FLAGS_HDR10METADATA;
    frame.SdrWhiteLevel = 203;
    frame.SurfaceColorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    frame.Hdr10FrameMetaData.NewMetaData.MaxContentLightLevel = 400;
    for (auto type : {IDDCX_HDR10_FRAME_METADATA_TYPE_DEFAULT, IDDCX_HDR10_FRAME_METADATA_TYPE_NEW,
                      IDDCX_HDR10_FRAME_METADATA_TYPE_UNCHANGED, IDDCX_HDR10_FRAME_METADATA_TYPE_DEFAULT}) {
        frame.Hdr10FrameMetaData.Type = type;
        color.Frame(frame);
        const auto capture = read();
        const auto& expected = type == IDDCX_HDR10_FRAME_METADATA_TYPE_DEFAULT ? hdr : frame.Hdr10FrameMetaData.NewMetaData;
        assert(capture.Color.FrameAvailable && capture.Color.HasHdr10Metadata && capture.Color.SdrWhiteNits == 203);
        assert(!memcmp(capture.Color.Hdr10Metadata, &expected, sizeof(expected)));
        if (type == IDDCX_HDR10_FRAME_METADATA_TYPE_NEW) {
            color.Commit(path);
            hdr.MaxContentLightLevel = 2000;
            assert(NT_SUCCESS(color.SetDefault(input)) && read().Generation == capture.Generation);
        }
    }
    frame.ValidFlags = IDDCX_METADATA2_VALID_FLAGS_NONE;
    color.Frame(frame);
    assert(!read().Color.HasHdr10Metadata);
    color.ResetFrames();
    assert(!read().Color.FrameAvailable);
}
#endif

int main() {
    for (auto check : {CheckProfiles, CheckTimingAndLegacyModes, CheckPreferredEdid, CheckVideoTimingExtension,
                       CheckRenderLimits, CheckCursorSetup, CheckCursor, CheckHdrOutput,
#if IDDCX_VERSION_MINOR >= 10
                       CheckVersionGate, CheckHdrMetadata,
#endif
                       CheckDriverDevice}) check();
    puts("Driver profile tests passed");
}
