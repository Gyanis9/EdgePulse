#include "Base/Security/RandomToken.h"

#include "Core/Crypto/Digest.h"
#include "Base/Security/RandomBytes.h"

#include <cstdint>
#include <vector>

namespace EdgePulse::Common
{
    std::string randomTokenHex(const std::size_t byteCount)
    {
        // 会话 token 与验证码 uuid 都是凭据：走私有随机源，不用公开随机数
        const std::vector<std::uint8_t> bytes = Common::randomSecretBytes(byteCount);
        return AsynGyanis::Core::Digest::toHex(bytes);
    }
} // namespace EdgePulse::Common
