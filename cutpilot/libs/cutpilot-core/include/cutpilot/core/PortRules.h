#pragma once

#include "cutpilot/core/Node.h"

namespace cutpilot::core {

// How a source output type lands on a target input type. Converted means the value
// flows through a permitted implicit conversion, which the canvas surfaces as a
// dotted connector tail near the target.
enum class PortMatch {
    Incompatible,
    Direct,
    Converted
};

// Whether data of type from may feed an input of type to. Control signals stay on
// control ports and never mix with data types, Any included. Any accepts and feeds
// every data type directly. The permitted implicit conversions are a mask used as
// an image and a number used as text.
PortMatch portMatch(PortType from, PortType to);

} // namespace cutpilot::core
