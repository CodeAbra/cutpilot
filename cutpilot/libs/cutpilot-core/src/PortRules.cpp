#include "cutpilot/core/PortRules.h"

namespace cutpilot::core {

PortMatch portMatch(PortType from, PortType to)
{
    const bool fromControl = (from == PortType::Control);
    const bool toControl = (to == PortType::Control);
    if (fromControl || toControl)
        return (fromControl && toControl) ? PortMatch::Direct : PortMatch::Incompatible;

    if (from == to)
        return PortMatch::Direct;
    if (from == PortType::Any || to == PortType::Any)
        return PortMatch::Direct;

    if (from == PortType::Mask && to == PortType::Image)
        return PortMatch::Converted;
    if (from == PortType::Number && to == PortType::Text)
        return PortMatch::Converted;

    return PortMatch::Incompatible;
}

} // namespace cutpilot::core
