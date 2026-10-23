#include "Core/Firmware/FirmwareSignatureVerifier.h"

#include <Core/Crypto/Digest.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    struct Options
    {
        std::filesystem::path file;                          ///< 待验固件
        std::string           signature;                     ///< Base64 签名
        std::string           keyId;                         ///< 信任键 key id
        std::filesystem::path keyDirectory{"config/firmware-keys"}; ///< 公钥目录
    };

    void printUsage()
    {
        std::cerr << "用法：FirmwareVerifier --file <固件> --signature <Base64> --key-id <key id> [--key-dir <公钥目录>]\n";
    }

    [[nodiscard]] std::optional<Options> parseOptions(const int argumentCount, char **argumentValues)
    {
        Options options;
        for (int index = 1; index < argumentCount; ++index)
        {
            const std::string_view argument = argumentValues[index];
            if (index + 1 >= argumentCount)
            {
                std::cerr << "参数缺少取值：" << argument << '\n';
                return std::nullopt;
            }
            const std::string value = argumentValues[++index];
            if (argument == "--file")
            {
                options.file = value;
            } else if (argument == "--signature")
            {
                options.signature = value;
            } else if (argument == "--key-id")
            {
                options.keyId = value;
            } else if (argument == "--key-dir")
            {
                options.keyDirectory = value;
            } else
            {
                std::cerr << "未知参数：" << argument << '\n';
                return std::nullopt;
            }
        }
        if (options.file.empty() || options.signature.empty() || options.keyId.empty())
        {
            std::cerr << "缺少 --file、--signature 或 --key-id\n";
            return std::nullopt;
        }
        return options;
    }
} // namespace

int main(const int argumentCount, char **argumentValues)
{
    const std::optional<Options> options = parseOptions(argumentCount, argumentValues);
    if (!options.has_value())
    {
        printUsage();
        return 1;
    }

    std::ifstream stream(options->file, std::ios::binary);
    if (!stream)
    {
        std::cerr << "无法打开固件文件：" << options->file << '\n';
        return 1;
    }
    // 样例只做一次性校验，直接把文件读成字符串交给摘要接口；生产侧上传走 FirmwareFileWriter 的增量摘要
    const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (!stream.good() && !stream.eof())
    {
        std::cerr << "读取固件文件失败：" << options->file << '\n';
        return 1;
    }

    try
    {
        const std::string digest = AsynGyanis::Core::Digest::sha256Hex(bytes);
        const auto        verifier = EdgePulse::Firmware::FirmwareSignatureVerifier::loadFromDirectory(options->keyDirectory);
        if (!verifier.has_value())
        {
            std::cerr << "固件信任键加载失败：" << verifier.error() << '\n';
            return 1;
        }
        if (verifier->empty())
        {
            std::cerr << "固件信任键目录为空：" << options->keyDirectory << '\n';
            return 1;
        }
        const auto verified = verifier->verify(options->keyId, digest, options->signature);
        if (!verified.has_value())
        {
            std::cerr << "固件验签失败：" << verified.error() << '\n';
            return 1;
        }
        std::cout << "verified\n";
        return 0;
    } catch (const std::exception &failure)
    {
        std::cerr << "固件验签异常：" << failure.what() << '\n';
        return 1;
    }
}
