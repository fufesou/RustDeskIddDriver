#include "Driver.h"

namespace display {
void CursorSnapshot::Prepare()
{
    pixels.resize(IDD_CURSOR_BYTES);
}

IDARG_IN_QUERY_HWCURSOR CursorSnapshot::Input()
{
    return {state.ShapeId, static_cast<UINT>(pixels.size()), pixels.data()};
}

NTSTATUS CursorSnapshot::Accept(const CursorUpdate& update)
{
    const auto& data = update.basic;
    if (data.IsCursorVisible && data.IsCursorShapeUpdated) {
        const auto& shape = data.CursorShapeInfo;
        if (shape.Width > IDD_CURSOR_EXTENT || shape.Height > IDD_CURSOR_EXTENT ||
            shape.Pitch < UINT64(shape.Width) * sizeof(UINT32) ||
            UINT64(shape.Height) * shape.Pitch > pixels.size()) return STATUS_DATA_ERROR;
        state.ShapeId = shape.ShapeId;
        state.Width = shape.Width;
        state.Height = shape.Height;
        state.Pitch = shape.Pitch;
        state.HotX = shape.XHot;
        state.HotY = shape.YHot;
        state.CursorType = shape.CursorType;
    }
    state.Visible = data.IsCursorVisible;
    state.PositionValid = data.IsCursorVisible && update.positionValid;
    state.PositionId = state.PositionValid ? update.positionId : 0;
    state.X = state.PositionValid ? data.X : 0;
    state.Y = state.PositionValid ? data.Y : 0;
    state.SdrWhiteNits = update.white;
    return STATUS_SUCCESS;
}

NTSTATUS CursorSnapshot::Write(const CtlCursorQuery& query, OutputBuffer& out) const
{
    auto header = state;
    header.ShapeUpdated = header.Visible && header.ShapeId != query.LastShapeId;
    const size_t bytes = header.ShapeUpdated ? size_t(header.Pitch) * header.Height : 0;
    if (out.size < sizeof(header) + bytes) return STATUS_BUFFER_TOO_SMALL;
    memcpy(out.data, &header, sizeof(header));
    if (bytes) memcpy(out.data + sizeof(header), pixels.data(), bytes);
    out.used = sizeof(header) + bytes;
    return STATUS_SUCCESS;
}
} // namespace display
