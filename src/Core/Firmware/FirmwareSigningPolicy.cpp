#include "Core/Firmware/FirmwareSigningPolicy.h"

namespace EdgePulse::Firmware
{
    bool FirmwareSigningPolicy::signatureRequired() const noexcept
    {
        return requireSignature;
    }
} // namespace EdgePulse::Firmware
