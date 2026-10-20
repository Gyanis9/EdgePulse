#include "Core/Firmware/FirmwareFileWriter.h"

#include "Base/Exception/EdgePulseException.h"

#include "Core/Coroutine/AsyncExecutor.h"
#include "Core/Crypto/Digest.h"

#include <openssl/evp.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace EdgePulse::Firmware
{
    struct FirmwareFileWriter::State
    {
        std::string   temporaryPath;   ///< 临时路径
        std::string   finalPath;       ///< 最终路径
        std::ofstream stream;          ///< 输出流
        EVP_MD_CTX   *digest{nullptr}; ///< SHA-256 上下文
        std::int64_t  sizeBytes{0};    ///< 已写字节
        bool          finished{false}; ///< 是否已收尾

        ~State()
        {
            if (stream.is_open())
            {
                stream.close();
            }
            if (digest != nullptr)
            {
                EVP_MD_CTX_free(digest);
            }
        }
    };

    FirmwareFileWriter::FirmwareFileWriter(std::shared_ptr<State> state) : m_state(std::move(state))
    {
    }

    FirmwareFileWriter::~FirmwareFileWriter() = default;

    AsynGyanis::Core::Task<std::shared_ptr<FirmwareFileWriter>> FirmwareFileWriter::open(AsynGyanis::Core::EventLoop &, std::string temporaryPath, std::string finalPath)
    {
        const std::filesystem::path parent = std::filesystem::path(temporaryPath).parent_path();
        if (!parent.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                throw Common::EdgePulseException("创建固件目录失败：" + parent.string() + "：" + error.message());
            }
        }
        auto state           = std::make_shared<State>();
        state->temporaryPath = std::move(temporaryPath);
        state->finalPath     = std::move(finalPath);
        state->stream.open(state->temporaryPath, std::ios::binary | std::ios::trunc);
        if (!state->stream.is_open())
        {
            throw Common::EdgePulseException("无法创建固件临时文件：" + state->temporaryPath);
        }
        state->digest = EVP_MD_CTX_new();
        if (state->digest == nullptr || EVP_DigestInit_ex(state->digest, EVP_sha256(), nullptr) != 1)
        {
            throw Common::EdgePulseException("初始化固件摘要失败");
        }
        co_return std::shared_ptr<FirmwareFileWriter>(new FirmwareFileWriter(std::move(state)));
    }

    AsynGyanis::Core::Task<bool> FirmwareFileWriter::append(AsynGyanis::Core::EventLoop &loop, std::string data)
    {
        co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<bool>(loop, std::function<bool()>(
                                                                                                [state = m_state, data = std::move(data)]() -> bool
                                                                                                {
                                                                                                    if (state->finished || !state->stream.is_open())
                                                                                                    {
                                                                                                        return false;
                                                                                                    }
                                                                                                    state->stream.write(data.data(), static_cast<std::streamsize>(data.size()));
                                                                                                    if (!state->stream)
                                                                                                    {
                                                                                                        return false;
                                                                                                    }
                                                                                                    if (EVP_DigestUpdate(state->digest, data.data(), data.size()) != 1)
                                                                                                    {
                                                                                                        return false;
                                                                                                    }
                                                                                                    state->sizeBytes += static_cast<std::int64_t>(data.size());
                                                                                                    return true;
                                                                                                }));
    }

    AsynGyanis::Core::Task<std::optional<std::string>> FirmwareFileWriter::finish(AsynGyanis::Core::EventLoop &loop)
    {
        co_return co_await AsynGyanis::Core::AsyncExecutor::shared().submit<std::optional<std::string>>(
                loop, std::function<std::optional<std::string>()>(
                              [state = m_state]() -> std::optional<std::string>
                              {
                                  if (state->finished || !state->stream.is_open())
                                  {
                                      return std::nullopt;
                                  }
                                  state->stream.flush();
                                  state->stream.close();
                                  if (!state->stream)
                                  {
                                      return std::nullopt;
                                  }
                                  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
                                  unsigned int                               length = 0;
                                  if (EVP_DigestFinal_ex(state->digest, digest.data(), &length) != 1)
                                  {
                                      return std::nullopt;
                                  }
                                  std::error_code error;
                                  std::filesystem::rename(state->temporaryPath, state->finalPath, error);
                                  if (error)
                                  {
                                      return std::nullopt;
                                  }
                                  state->finished = true;
                                  return AsynGyanis::Core::Digest::toHex(std::span<const std::uint8_t>(digest.data(), length));
                              }));
    }

    void FirmwareFileWriter::abort() noexcept
    {
        if (!m_state)
        {
            return;
        }
        if (m_state->stream.is_open())
        {
            m_state->stream.close();
        }
        std::error_code error;
        std::filesystem::remove(m_state->temporaryPath, error);
        std::filesystem::remove(m_state->finalPath, error);
        m_state->finished = true;
    }

    std::int64_t FirmwareFileWriter::size() const noexcept
    {
        return m_state ? m_state->sizeBytes : 0;
    }
} // namespace EdgePulse::Firmware
