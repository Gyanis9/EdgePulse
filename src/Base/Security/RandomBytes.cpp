#include "Base/Security/RandomBytes.h"

#include "Base/Exception/EdgePulseException.h"

#include <openssl/rand.h>

#include <cstddef>
#include <vector>

namespace EdgePulse::Common
{
    std::vector<std::uint8_t> randomBytes(const std::size_t byteCount)
    {
        std::vector<std::uint8_t> bytes(byteCount);
        if (byteCount == 0)
        {
            return bytes;
        }
        // RAND_bytes 失败时不会写满缓冲；抛出而不是交出半填的“随机”值
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        {
            throw EdgePulseException("随机源取不出字节：OpenSSL RAND_bytes 失败");
        }
        return bytes;
    }

    std::vector<std::uint8_t> randomSecretBytes(const std::size_t byteCount)
    {
        std::vector<std::uint8_t> bytes(byteCount);
        if (byteCount == 0)
        {
            return bytes;
        }
        // 密钥材料走私有 DRBG，与公开随机数分流
        if (RAND_priv_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        {
            throw EdgePulseException("随机源取不出密钥材料：OpenSSL RAND_priv_bytes 失败");
        }
        return bytes;
    }
} // namespace EdgePulse::Common
