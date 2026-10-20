#include "Common/PasswordHash.h"

#include "Base/Exception/Exception.h"
#include "Base/Coding/Base64.h"
#include "Base/Coding/SecureCompare.h"
#include "Common/RandomBytes.h"

#include <optional>
#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace EdgePulse::Common
{
    namespace
    {
        constexpr std::string_view kAlgorithmName = "pbkdf2_sha256";

        /**
         * @brief 按 '$' 拆成四段
         * @param text 待拆文本
         * @return 恰好四段时返回；否则为空
         */
        [[nodiscard]] std::vector<std::string_view> splitHashText(const std::string_view text)
        {
            std::vector<std::string_view> parts;
            std::size_t                   begin = 0;
            while (begin <= text.size())
            {
                const std::size_t separator = text.find('$', begin);
                if (separator == std::string_view::npos)
                {
                    parts.push_back(text.substr(begin));
                    break;
                }
                parts.push_back(text.substr(begin, separator - begin));
                begin = separator + 1;
            }
            return parts;
        }

        /**
         * @brief 算一次 PBKDF2
         * @param password 明文
         * @param salt 盐字节
         * @param iterations 迭代次数
         * @param output 输出缓冲
         * @return 是否成功
         */
        [[nodiscard]] bool computePbkdf2(const std::string_view password, const std::string_view salt, const int iterations, std::array<unsigned char, kPbkdf2HashBytes> &output)
        {
            const int result = PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), reinterpret_cast<const unsigned char *>(salt.data()),
                                                 static_cast<int>(salt.size()), iterations, EVP_sha256(), static_cast<int>(output.size()), output.data());
            return result == 1;
        }
    } // namespace

    std::string hashPassword(const std::string_view password)
    {
        // 盐走私有随机源：它与口令一起构成凭据材料，不能用公开随机数
        const std::vector<std::uint8_t> saltBytes = Common::randomSecretBytes(kPbkdf2SaltBytes);
        const std::string               salt(reinterpret_cast<const char *>(saltBytes.data()), saltBytes.size());

        std::array<unsigned char, kPbkdf2HashBytes> output{};
        if (!computePbkdf2(password, salt, kPbkdf2Iterations, output))
        {
            throw AsynGyanis::Base::Exception("PBKDF2 计算失败：OpenSSL 没有产出完整摘要");
        }

        const std::string saltText(reinterpret_cast<const char *>(salt.data()), salt.size());
        const std::string hashText(reinterpret_cast<const char *>(output.data()), output.size());
        return std::string{kAlgorithmName} + "$" + std::to_string(kPbkdf2Iterations) + "$" + AsynGyanis::Base::base64Encode(saltText) + "$" +
               AsynGyanis::Base::base64Encode(hashText);
    }

    bool verifyPassword(const std::string_view storedHash, const std::string_view password)
    {
        const std::vector<std::string_view> parts = splitHashText(storedHash);
        // 段数、算法名、迭代数任一不符都按“校验失败”处理，不把格式细节回给调用方
        if (parts.size() != 4 || parts[0] != kAlgorithmName)
        {
            return false;
        }
        int iterations          = 0;
        const auto [end, error] = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), iterations);
        if (error != std::errc{} || end != parts[1].data() + parts[1].size() || iterations <= 0)
        {
            return false;
        }

        const std::optional<std::string> salt         = AsynGyanis::Base::base64Decode(parts[2]);
        const std::optional<std::string> expectedHash = AsynGyanis::Base::base64Decode(parts[3]);
        if (!salt.has_value() || !expectedHash.has_value() || salt->size() != kPbkdf2SaltBytes || expectedHash->size() != kPbkdf2HashBytes)
        {
            return false;
        }

        std::array<unsigned char, kPbkdf2HashBytes> output{};
        if (!computePbkdf2(password, *salt, iterations, output))
        {
            return false;
        }
        const std::string_view computed(reinterpret_cast<const char *>(output.data()), output.size());
        return AsynGyanis::Base::constantTimeEquals(computed, *expectedHash);
    }
} // namespace EdgePulse::Common
