#include "Core/Device/EnrollmentService.h"

#include "Base/Runtime/TimeFormat.h"
#include "Base/Security/RandomToken.h"

#include <Core/Crypto/Digest.h>

#include <chrono>
#include <string>
#include <utility>

namespace EdgePulse::Device
{
    namespace
    {
        constexpr std::size_t kMaximumNodeNameLength = 128;
        constexpr const char *kPlaintextPrefix       = "ep_enroll_";
    } // namespace

    EnrollmentService::EnrollmentService(std::shared_ptr<EnrollmentRepository> repository) : m_repository(std::move(repository))
    {
    }

    std::shared_ptr<EnrollmentService> EnrollmentService::scoped(const System::TenantScope tenant) const
    {
        return std::make_shared<EnrollmentService>(m_repository->scoped(tenant));
    }

    AsynGyanis::Core::Task<std::expected<IssuedEnrollmentToken, std::string>> EnrollmentService::create(AsynGyanis::Core::EventLoop &loop, const std::int64_t tenantId,
                                                                                                        std::string nodeName, std::string action, const int expiresInSeconds,
                                                                                                        const std::int64_t operatorId)
    {
        if (tenantId <= 0)
        {
            co_return std::unexpected("令牌必须归属一个有效租户");
        }
        if (action != "enroll" && action != "rebind")
        {
            co_return std::unexpected("令牌动作只能是 enroll 或 rebind");
        }
        if (nodeName.size() > kMaximumNodeNameLength)
        {
            co_return std::unexpected("节点名不能超过 128 个字符");
        }
        if (expiresInSeconds < kEnrollmentMinimumExpirySeconds || expiresInSeconds > kEnrollmentMaximumExpirySeconds)
        {
            co_return std::unexpected("有效期必须在 300 到 2592000 秒之间");
        }
        const std::string     plaintext = std::string{kPlaintextPrefix} + Common::randomTokenHex(32);
        EnrollmentTokenRecord record;
        record.tenantId                    = tenantId;
        record.nodeName                    = std::move(nodeName);
        record.action                      = std::move(action);
        record.expiresAt                   = Common::formatDateTimeUtc(std::chrono::system_clock::now() + std::chrono::seconds{expiresInSeconds});
        record.createdBy                   = operatorId;
        record.createdAt                   = Common::nowDateTimeUtc();
        const EnrollmentTokenRecord stored = co_await m_repository->create(loop, record, hashToken(plaintext));
        co_return IssuedEnrollmentToken{stored, plaintext};
    }

    AsynGyanis::Core::Task<std::vector<EnrollmentTokenRecord>> EnrollmentService::list(AsynGyanis::Core::EventLoop &loop, std::string status)
    {
        co_return co_await m_repository->list(loop, std::move(status));
    }

    AsynGyanis::Core::Task<bool> EnrollmentService::revoke(AsynGyanis::Core::EventLoop &loop, const std::int64_t tokenId)
    {
        co_return co_await m_repository->revoke(loop, tokenId);
    }

    std::string EnrollmentService::hashToken(const std::string_view token)
    {
        return AsynGyanis::Core::Digest::sha256Hex(token);
    }
} // namespace EdgePulse::Device
