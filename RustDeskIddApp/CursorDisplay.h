#pragma once

#include "../RustDeskIddDriver/DisplayProfile.h"
#include <ostream>

inline void WriteCursorPosition(std::ostream& out, const CtlCursorState& cursor)
{
    if (cursor.PositionValid) out << "x=" << cursor.X << " y=" << cursor.Y;
    else out << "position unavailable";
}
