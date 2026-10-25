#include "AgentCertificateManager.h"

#include "Platform/FileSystem/AtomicFileWriter.h"
#include "Platform/FileSystem/FileSystem.h"
#include "Platform/System/ProcessInfo.h"

#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPolicy.h"

#include "Base/Log/LogMacros.h"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace EdgePulse::Agent
{
    namespace
    {
        using BioHandle        = std::unique_ptr<BIO, decltype(&BIO_free)>;
        using KeyHandle        = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
        using CertHandle       = std::unique_ptr<X509, decltype(&X509_free)>;
        using RequestHandle    = std::unique_ptr<X509_REQ, decltype(&X509_REQ_free)>;
        using StoreHandle      = std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)>;
        using StoreContext     = std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)>;
        using CertificateStack = std::unique_ptr<STACK_OF(X509), void (*)(STACK_OF(X509) *)>;

        constexpr std::filesystem::perms kPrivateKeyPermissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;

        /**
         * @brief 释放证书栈里的每张证书与栈本身
         * @param stack 证书栈，可为空
         */
        void freeCertificateStack(STACK_OF(X509) * stack)
        {
            if (stack != nullptr)
            {
                sk_X509_pop_free(stack, X509_free);
            }
        }

        /**
         * @brief 把内存 BIO 里的 PEM 文本取成字符串
         * @param bio 内存 BIO
         * @return 成功返回 PEM 文本；为空时返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> pemTextOf(BIO &bio)
        {
            char      *buffer = nullptr;
            const long length = BIO_get_mem_data(&bio, &buffer);
            if (buffer == nullptr || length <= 0)
            {
                return std::unexpected("PEM 输出为空");
            }
            return std::string{buffer, static_cast<std::size_t>(length)};
        }

        /**
         * @brief 读取文件正文
         * @param path 文件路径
         * @return 成功返回正文；打不开时返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> readTextFile(const std::filesystem::path &path)
        {
            std::ifstream stream(path, std::ios::binary);
            if (!stream.is_open())
            {
                return std::unexpected("无法读取现有文件：" + path.string());
            }
            std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            if (stream.bad())
            {
                return std::unexpected("读取现有文件失败：" + path.string());
            }
            return text;
        }

        /**
         * @brief 检查文件是否存在
         * @param path 文件路径
         * @return bool 存在且不是目录时返回 true
         */
        [[nodiscard]] bool fileExists(const std::filesystem::path &path)
        {
            std::error_code error;
            return std::filesystem::is_regular_file(path, error) && !error;
        }

        /**
         * @brief 检查 PEM 文本里只有证书块
         * @param certificatePem 服务端返回的 PEM 文本
         * @return 成功返回证书块个数；出现其他标签或没有证书块时返回中文原因
         * @details PEM 读取只挑 CERTIFICATE 块，其余块会被静默跳过；不先查标签就会把夹带私钥或
         *          其他材料的响应当成一份干净证书收下
         */
        [[nodiscard]] std::expected<std::size_t, std::string> certificateBlockCount(const std::string_view certificatePem)
        {
            constexpr std::string_view kBeginMarker = "-----BEGIN ";
            constexpr std::string_view kLabelEnd    = "-----";
            std::size_t                count        = 0;
            std::size_t                position     = 0;
            for (;;)
            {
                const std::size_t begin = certificatePem.find(kBeginMarker, position);
                if (begin == std::string_view::npos)
                {
                    break;
                }
                const std::size_t labelBegin = begin + kBeginMarker.size();
                const std::size_t labelEnd   = certificatePem.find(kLabelEnd, labelBegin);
                if (labelEnd == std::string_view::npos)
                {
                    return std::unexpected("服务端返回的证书里有一个没有结束标记的 PEM 段落");
                }
                const std::string_view label = certificatePem.substr(labelBegin, labelEnd - labelBegin);
                if (label != "CERTIFICATE")
                {
                    return std::unexpected("服务端返回的证书包含非证书 PEM 段落（" + std::string{label} + "）");
                }
                ++count;
                position = labelEnd + kLabelEnd.size();
            }
            if (count == 0)
            {
                return std::unexpected("服务端返回的证书不是 PEM 编码的 X.509 证书");
            }
            return count;
        }

        /**
         * @brief 解析证书链 PEM
         * @param certificatePem 服务端返回的 PEM 文本
         * @return 成功返回证书列表，第一张是叶证书；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::vector<CertHandle>, std::string> parseCertificateChain(const std::string_view certificatePem)
        {
            if (certificatePem.empty())
            {
                return std::unexpected("服务端返回的证书为空");
            }
            const auto expectedCount = certificateBlockCount(certificatePem);
            if (!expectedCount)
            {
                return std::unexpected(expectedCount.error());
            }
            BioHandle certificateBio(BIO_new_mem_buf(certificatePem.data(), static_cast<int>(certificatePem.size())), &BIO_free);
            if (!certificateBio)
            {
                return std::unexpected("无法创建证书解析缓冲");
            }

            std::vector<CertHandle> certificates;
            for (;;)
            {
                CertHandle certificate(PEM_read_bio_X509(certificateBio.get(), nullptr, nullptr, nullptr), &X509_free);
                if (!certificate)
                {
                    ERR_clear_error();
                    break;
                }
                certificates.push_back(std::move(certificate));
            }
            if (certificates.size() != *expectedCount)
            {
                return std::unexpected("服务端返回的证书链里有 " + std::to_string(*expectedCount) + " 个证书块，但只能解析出 " + std::to_string(certificates.size()) + " 张证书");
            }
            return certificates;
        }

        /**
         * @brief 把解析出的证书链重新编码成 PEM
         * @param certificates 证书链，第一张是叶证书
         * @return 成功返回规范化的证书链 PEM；失败返回中文原因
         */
        [[nodiscard]] std::expected<std::string, std::string> encodeCertificateChain(const std::vector<CertHandle> &certificates)
        {
            BioHandle outputBio(BIO_new(BIO_s_mem()), &BIO_free);
            if (!outputBio)
            {
                return std::unexpected("无法创建证书输出缓冲");
            }
            for (const auto &certificate: certificates)
            {
                if (PEM_write_bio_X509(outputBio.get(), certificate.get()) != 1)
                {
                    ERR_clear_error();
                    return std::unexpected("重新编码证书链失败");
                }
            }
            return pemTextOf(*outputBio);
        }

        /**
         * @brief 用配置的 CA 校验证书链
         * @param certificates 证书链，第一张是叶证书
         * @param caFile 信任锚路径
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> verifyCertificateChain(const std::vector<CertHandle> &certificates, const std::filesystem::path &caFile)
        {
            StoreHandle store(X509_STORE_new(), &X509_STORE_free);
            if (!store)
            {
                return std::unexpected("创建证书信任库失败");
            }
            if (X509_STORE_load_file(store.get(), caFile.string().c_str()) != 1)
            {
                ERR_clear_error();
                return std::unexpected("无法加载 CA 文件：" + caFile.string());
            }

            CertificateStack untrusted(nullptr, &freeCertificateStack);
            if (certificates.size() > 1)
            {
                STACK_OF(X509) *stack = sk_X509_new_null();
                if (stack == nullptr)
                {
                    return std::unexpected("创建中间证书栈失败");
                }
                untrusted.reset(stack);
                for (std::size_t index = 1; index < certificates.size(); ++index)
                {
                    if (X509_up_ref(certificates[index].get()) != 1)
                    {
                        return std::unexpected("保留中间证书失败");
                    }
                    if (sk_X509_push(stack, certificates[index].get()) == 0)
                    {
                        X509_free(certificates[index].get());
                        return std::unexpected("写入中间证书栈失败");
                    }
                }
            }

            StoreContext context(X509_STORE_CTX_new(), &X509_STORE_CTX_free);
            if (!context)
            {
                return std::unexpected("创建证书校验上下文失败");
            }
            if (X509_STORE_CTX_init(context.get(), store.get(), certificates.front().get(), untrusted.get()) != 1)
            {
                ERR_clear_error();
                return std::unexpected("初始化证书校验失败");
            }
            if (X509_verify_cert(context.get()) != 1)
            {
                const int         reason = X509_STORE_CTX_get_error(context.get());
                const char *const text   = X509_verify_cert_error_string(reason);
                return std::unexpected("服务端返回的证书无法链接到配置的 CA（" + std::string{text != nullptr ? text : "未知原因"} + "）");
            }
            return {};
        }

        /**
         * @brief 检查证书是否仍然有效
         * @param certificate 叶证书
         * @return 成功为空；已过期或时间无法解析时返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> verifyNotExpired(X509 &certificate)
        {
            // X509_cmp_current_time：证书到期时间晚于当前时刻返回 1，早于返回 -1，相等或解析失败返回 0
            const int comparison = X509_cmp_current_time(X509_get0_notAfter(&certificate));
            if (comparison <= 0)
            {
                return std::unexpected("服务端返回的证书已过期或到期时间无法解析（notAfter 不晚于当前时刻）");
            }
            return {};
        }

        /**
         * @brief 把 ASN.1 时间转成 UTC 时间戳
         * @param value 证书里的时间字段
         * @return 成功返回时间戳；格式无法解析或换算失败时为空
         */
        [[nodiscard]] std::optional<std::time_t> utcTimeOf(const ASN1_TIME *value)
        {
            tm parsed{};
            if (value == nullptr || ASN1_TIME_to_tm(value, &parsed) != 1)
            {
                return std::nullopt;
            }
#ifdef _WIN32
            const std::time_t seconds = _mkgmtime(&parsed);
#else
            const std::time_t seconds = timegm(&parsed);
#endif
            if (seconds == static_cast<std::time_t>(-1))
            {
                return std::nullopt;
            }
            return seconds;
        }

        /**
         * @brief 检查证书剩余有效期是否超过续期提前窗口
         * @param certificate 叶证书
         * @param renewBeforeDays 配置的续期提前天数覆盖值，0 表示按规范公式
         * @return 成功为空；剩余有效期不足以跨过窗口时返回中文原因
         * @details 窗口取 max(25% 生命周期, 24h)，生命周期由证书自身的 notBefore 与 notAfter 决定；
         *          配置的覆盖值只在小于生命周期时生效。覆盖值不小于生命周期说明服务端有效期与
         *          Agent 配置互相矛盾，按不可重试的配置错误拒绝安装，避免装好后立刻又进入续期窗口。
         */
        [[nodiscard]] std::expected<void, std::string> verifyLifetimeBeyondRenewWindow(X509 &certificate, const int renewBeforeDays)
        {
            const auto start = utcTimeOf(X509_get0_notBefore(&certificate));
            const auto end   = utcTimeOf(X509_get0_notAfter(&certificate));
            if (!start.has_value() || !end.has_value())
            {
                return std::unexpected("服务端返回的证书生效或到期时间无法解析");
            }
            const auto lifetime = std::chrono::seconds{*end - *start};
            if (lifetime <= std::chrono::seconds::zero())
            {
                return std::unexpected("服务端返回的证书有效期不是正数，拒绝安装");
            }
            if (renewBeforeDays > 0 && !renewalWindowOverrideValid(lifetime, renewBeforeDays))
            {
                const auto lifetimeHours = std::chrono::duration_cast<std::chrono::hours>(lifetime).count();
                return std::unexpected("服务端返回的证书有效期 " + std::to_string(lifetimeHours) + " 小时，不大于配置的续期提前量 " + std::to_string(renewBeforeDays) +
                                       " 天（agent.certificate.renew_before_days）；请把它设为 0 或小于证书有效期");
            }
            const auto window    = renewalWindow(lifetime, renewBeforeDays);
            const auto remaining = std::chrono::system_clock::from_time_t(*end) - std::chrono::system_clock::now();
            if (remaining <= window)
            {
                const auto windowHours = std::chrono::duration_cast<std::chrono::hours>(window).count();
                return std::unexpected("服务端返回的证书剩余有效期不超过续期提前量（" + std::to_string(windowHours) + " 小时），拒绝安装");
            }
            return {};
        }

        /**
         * @brief 用临时 TLS 上下文校验一对证书与私钥文件能装载
         * @param configuration 配置，只用 CA 路径
         * @param certificateFile 待验证的证书文件
         * @param privateKeyFile 待验证的私钥文件
         * @return 成功为空；失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> verifyTlsIdentity(const AgentConfiguration &configuration, const std::filesystem::path &certificateFile,
                                                                         const std::filesystem::path &privateKeyFile)
        {
            try
            {
                AsynGyanis::Core::TlsPolicy policy;
                if (!configuration.caFile.empty())
                {
                    policy.certificateAuthorityFile = configuration.caFile.string();
                }
                AsynGyanis::Core::TlsContext context(policy, AsynGyanis::Core::TlsContext::Role::Client);
                if (!context.loadCertificate(certificateFile.string(), privateKeyFile.string()))
                {
                    ERR_clear_error();
                    return std::unexpected("新证书与私钥无法装载到 TLS 上下文：请确认两者配对且均为 PEM 编码");
                }
                return {};
            } catch (const std::exception &failure)
            {
                return std::unexpected("创建临时 TLS 上下文失败：" + std::string{failure.what()});
            }
        }

        /**
         * @brief 取一次安装中断标记记录的路径
         */
        struct InstallMarker
        {
            std::filesystem::path certificate;       ///< 正式证书路径
            std::filesystem::path certificateBackup; ///< 安装前证书的备份路径
            std::filesystem::path privateKey;        ///< 正式私钥路径
            std::filesystem::path privateKeyBackup;  ///< 安装前私钥的备份路径
        };

        /**
         * @brief 取安装中断标记的路径
         * @param configuration 身份配置
         * @return std::filesystem::path 与证书同目录的标记文件
         */
        [[nodiscard]] std::filesystem::path installMarkerPath(const AgentConfiguration &configuration)
        {
            return configuration.certificateFile.string() + ".install-pending";
        }

        /**
         * @brief 拼出安装中断标记的正文
         * @param configuration 身份配置
         * @return std::string 四行路径清单，路径按 UTF-8 编码
         */
        [[nodiscard]] std::string installMarkerText(const AgentConfiguration &configuration)
        {
            const auto backupOf = [](const std::filesystem::path &path) { return std::filesystem::path{path.string() + ".bak"}; };
            const auto utf8Of   = [](const std::filesystem::path &path) { return AsynGyanis::Platform::FileSystem::utf8FromPath(path); };
            return "certificate=" + utf8Of(configuration.certificateFile) + "\n" + "certificate_backup=" + utf8Of(backupOf(configuration.certificateFile)) + "\n" +
                   "private_key=" + utf8Of(configuration.privateKeyFile) + "\n" + "private_key_backup=" + utf8Of(backupOf(configuration.privateKeyFile)) + "\n";
        }

        /**
         * @brief 读安装中断标记
         * @param markerPath 标记路径
         * @return 成功返回四项路径；文件打不开或内容不完整时返回中文原因
         */
        [[nodiscard]] std::expected<InstallMarker, std::string> readInstallMarker(const std::filesystem::path &markerPath)
        {
            std::ifstream stream(markerPath, std::ios::binary);
            if (!stream.is_open())
            {
                return std::unexpected("无法读取证书安装标记：" + AsynGyanis::Platform::FileSystem::utf8FromPath(markerPath));
            }

            InstallMarker marker;
            bool          hasCertificate       = false;
            bool          hasCertificateBackup = false;
            bool          hasPrivateKey        = false;
            bool          hasPrivateKeyBackup  = false;
            std::string   line;
            while (std::getline(stream, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                {
                    line.pop_back();
                }
                const std::size_t separator = line.find('=');
                if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size())
                {
                    continue;
                }
                const std::string key   = line.substr(0, separator);
                const auto        value = AsynGyanis::Platform::FileSystem::pathFromUtf8(line.substr(separator + 1));
                if (key == "certificate")
                {
                    marker.certificate = value;
                    hasCertificate     = true;
                } else if (key == "certificate_backup")
                {
                    marker.certificateBackup = value;
                    hasCertificateBackup     = true;
                } else if (key == "private_key")
                {
                    marker.privateKey = value;
                    hasPrivateKey     = true;
                } else if (key == "private_key_backup")
                {
                    marker.privateKeyBackup = value;
                    hasPrivateKeyBackup     = true;
                }
            }
            if (!hasCertificate || !hasCertificateBackup || !hasPrivateKey || !hasPrivateKeyBackup)
            {
                return std::unexpected("证书安装标记内容不完整：" + AsynGyanis::Platform::FileSystem::utf8FromPath(markerPath));
            }
            return marker;
        }

        /**
         * @brief 删除一个文件，不存在视为成功
         * @param path 目标路径
         * @param label 写进失败原因的名字
         * @return 成功为空；删除失败返回中文原因
         */
        [[nodiscard]] std::expected<void, std::string> removeFileChecked(const std::filesystem::path &path, const std::string_view label)
        {
            std::error_code error;
            std::filesystem::remove(path, error);
            if (error)
            {
                return std::unexpected("删除" + std::string{label} + "失败：" + AsynGyanis::Platform::FileSystem::utf8FromPath(path) + "：" + error.message());
            }
            return {};
        }

        /**
         * @brief 原子写入私钥，避免临时文件在设置权限前被其他账户读到
         * @param targetPath 正式私钥路径
         * @param privateKeyPem 私钥 PEM 正文
         * @return 成功为空；失败返回中文原因
         * @details AtomicFileWriter 先按进程默认权限创建临时文件，写完才改成 0600。
         *          POSIX 上目录若允许同组读取，这段窗口里私钥可被读到。这里先在同级建 0700
         *          暂存目录，在目录内完成临时写入与改名，再把结果改名到正式路径；暂存目录本身
         *          挡住其他账户，写入结束后递归删除。Windows 的目录 ACL 已限定服务账户，直接写。
         */
        [[nodiscard]] std::expected<void, std::string> writePrivateKeyFile(const std::filesystem::path &targetPath, const std::string &privateKeyPem)
        {
            std::string writeError;
#ifdef _WIN32
            if (!AsynGyanis::Platform::AtomicFileWriter::writeText(targetPath, privateKeyPem, kPrivateKeyPermissions, &writeError))
            {
                return std::unexpected(writeError);
            }
            return {};
#else
            static std::atomic<std::uint32_t> stagingCounter{0};
            const auto stagingDirectory       = targetPath.parent_path() / (".private-key-stage." + std::to_string(AsynGyanis::Platform::ProcessInfo::currentProcessId()) + "." +
                                                                            std::to_string(stagingCounter.fetch_add(1, std::memory_order_relaxed)));
            const auto removeStagingDirectory = [&stagingDirectory]()
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(stagingDirectory, cleanupError);
            };

            std::error_code directoryError;
            std::filesystem::create_directories(stagingDirectory, directoryError);
            if (directoryError)
            {
                return std::unexpected("创建私钥暂存目录失败：" + AsynGyanis::Platform::FileSystem::utf8FromPath(stagingDirectory) + "：" + directoryError.message());
            }
            std::filesystem::permissions(stagingDirectory, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, directoryError);
            if (directoryError)
            {
                removeStagingDirectory();
                return std::unexpected("设置私钥暂存目录权限失败：" + AsynGyanis::Platform::FileSystem::utf8FromPath(stagingDirectory) + "：" + directoryError.message());
            }

            const auto stagedPath = stagingDirectory / targetPath.filename();
            if (!AsynGyanis::Platform::AtomicFileWriter::writeText(stagedPath, privateKeyPem, kPrivateKeyPermissions, &writeError))
            {
                removeStagingDirectory();
                return std::unexpected(writeError);
            }
            std::filesystem::rename(stagedPath, targetPath, directoryError);
            removeStagingDirectory();
            if (directoryError)
            {
                return std::unexpected("替换私钥文件失败：" + AsynGyanis::Platform::FileSystem::utf8FromPath(targetPath) + "：" + directoryError.message());
            }
            return {};
#endif
        }
    } // namespace

    std::chrono::seconds renewalWindow(const std::chrono::seconds lifetime, const int renewBeforeDaysOverride)
    {
        if (renewBeforeDaysOverride > 0)
        {
            const auto configured = std::chrono::hours{24} * renewBeforeDaysOverride;
            // 生命周期未知时无法比较，仍按配置执行；已知且覆盖值不小于生命周期时退回规范公式
            if (lifetime <= std::chrono::seconds::zero() || configured < lifetime)
            {
                return configured;
            }
        }
        if (lifetime <= std::chrono::seconds::zero())
        {
            return std::chrono::seconds{kMinimumRenewalWindow};
        }
        // 规范窗口：max(25% 生命周期, 24h)
        return std::max(lifetime / 4, std::chrono::seconds{kMinimumRenewalWindow});
    }

    bool renewalWindowOverrideValid(const std::chrono::seconds lifetime, const int renewBeforeDaysOverride)
    {
        if (renewBeforeDaysOverride <= 0)
        {
            return true;
        }
        // 生命周期未知时没有比较依据，按配置执行；已知时覆盖值必须严格小于生命周期
        return lifetime <= std::chrono::seconds::zero() || std::chrono::hours{24} * renewBeforeDaysOverride < lifetime;
    }

    bool permanentInstallFailure(const std::string_view reason)
    {
        return reason.find("无法链接到配置的 CA") != std::string_view::npos || reason.find("与本次生成的私钥不匹配") != std::string_view::npos ||
               reason.find("剩余有效期不超过续期提前量") != std::string_view::npos || reason.find("不大于配置的续期提前量") != std::string_view::npos ||
               reason.find("生效或到期时间无法解析") != std::string_view::npos || reason.find("有效期不是正数") != std::string_view::npos ||
               reason.find("已过期") != std::string_view::npos || reason.find("证书或私钥路径为空") != std::string_view::npos;
    }

    AgentCertificateManager::AgentCertificateManager(AgentConfiguration configuration) : m_configuration(std::move(configuration))
    {
    }

    std::expected<CertificateRequestMaterial, std::string> AgentCertificateManager::generateKeyAndCsr()
    {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), &EVP_PKEY_CTX_free);
        if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) != 1 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext.get(), NID_X9_62_prime256v1) != 1)
        {
            ERR_clear_error();
            return std::unexpected("生成 EC 私钥失败：无法初始化 P-256 密钥参数");
        }
        EVP_PKEY *rawKey = nullptr;
        KeyHandle key(nullptr, &EVP_PKEY_free);
        if (EVP_PKEY_keygen(keyContext.get(), &rawKey) != 1 || rawKey == nullptr)
        {
            ERR_clear_error();
            return std::unexpected("生成 EC 私钥失败：OpenSSL 没有返回密钥");
        }
        key.reset(rawKey);

        RequestHandle request(X509_REQ_new(), &X509_REQ_free);
        if (!request)
        {
            return std::unexpected("创建证书签名请求失败");
        }
        if (X509_REQ_set_version(request.get(), 0) != 1 || X509_REQ_set_pubkey(request.get(), key.get()) != 1)
        {
            ERR_clear_error();
            return std::unexpected("填写证书签名请求失败");
        }

        const std::string commonName = m_configuration.name.empty() ? std::string{"edgepulse-agent"} : m_configuration.name;
        X509_NAME        *subject    = X509_REQ_get_subject_name(request.get());
        if (subject == nullptr || X509_NAME_add_entry_by_txt(subject, SN_commonName, MBSTRING_UTF8, reinterpret_cast<const unsigned char *>(commonName.c_str()), -1, -1, 0) != 1)
        {
            ERR_clear_error();
            return std::unexpected("写入证书签名请求主题失败");
        }
        if (X509_REQ_sign(request.get(), key.get(), EVP_sha256()) <= 0)
        {
            ERR_clear_error();
            return std::unexpected("签名证书签名请求失败");
        }

        BioHandle keyBio(BIO_new(BIO_s_mem()), &BIO_free);
        if (!keyBio || PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
        {
            ERR_clear_error();
            return std::unexpected("输出私钥 PEM 失败");
        }
        const auto privateKeyPem = pemTextOf(*keyBio);
        if (!privateKeyPem)
        {
            return std::unexpected("输出私钥 PEM 失败：" + privateKeyPem.error());
        }

        BioHandle requestBio(BIO_new(BIO_s_mem()), &BIO_free);
        if (!requestBio || PEM_write_bio_X509_REQ(requestBio.get(), request.get()) != 1)
        {
            ERR_clear_error();
            return std::unexpected("输出证书签名请求 PEM 失败");
        }
        const auto csrPem = pemTextOf(*requestBio);
        if (!csrPem)
        {
            return std::unexpected("输出证书签名请求 PEM 失败：" + csrPem.error());
        }

        m_pending.privateKeyPem = *privateKeyPem;
        m_pending.csrPem        = *csrPem;
        return m_pending;
    }

    std::expected<AgentIdentity, std::string> AgentCertificateManager::install(const std::string_view certificatePem)
    {
        if (m_pending.privateKeyPem.empty())
        {
            return std::unexpected("没有待安装的私钥：请先生成本次续期的密钥与 CSR");
        }

        const auto certificates = parseCertificateChain(certificatePem);
        if (!certificates)
        {
            return std::unexpected(certificates.error());
        }
        X509 &leaf = *certificates->front();
        if (const auto expired = verifyNotExpired(leaf); !expired)
        {
            return std::unexpected(expired.error());
        }
        if (const auto lifetime = verifyLifetimeBeyondRenewWindow(leaf, m_configuration.certificateRenewBeforeDays); !lifetime)
        {
            return std::unexpected(lifetime.error());
        }

        // 只把解析出的证书块写回磁盘：响应里夹带的说明文字或其他材料一律不进正式文件
        const auto encodedChain = encodeCertificateChain(*certificates);
        if (!encodedChain)
        {
            return std::unexpected(encodedChain.error());
        }

        BioHandle keyBio(BIO_new_mem_buf(m_pending.privateKeyPem.data(), static_cast<int>(m_pending.privateKeyPem.size())), &BIO_free);
        if (!keyBio)
        {
            return std::unexpected("无法创建待安装私钥的解析缓冲");
        }
        KeyHandle privateKey(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), &EVP_PKEY_free);
        if (!privateKey)
        {
            ERR_clear_error();
            return std::unexpected("待安装的私钥无法解析");
        }
        if (X509_check_private_key(&leaf, privateKey.get()) != 1)
        {
            ERR_clear_error();
            return std::unexpected("服务端返回的证书与本次生成的私钥不匹配");
        }
        if (const auto verified = verifyCertificateChain(*certificates, m_configuration.caFile); !verified)
        {
            return std::unexpected(verified.error());
        }

        const std::filesystem::path certificateFile = m_configuration.certificateFile;
        const std::filesystem::path privateKeyFile  = m_configuration.privateKeyFile;
        if (certificateFile.empty() || privateKeyFile.empty())
        {
            return std::unexpected("证书或私钥路径为空，无法安装新身份");
        }

        // 先在旁边落两份临时文件，用临时 TLS 上下文确认这对材料能装载；此时两份正式文件都还没动
        const std::filesystem::path temporaryCertificate = certificateFile.string() + ".renew-check";
        const std::filesystem::path temporaryPrivateKey  = privateKeyFile.string() + ".renew-check";
        std::string                 writeError;
        const auto                  removeTemporary = [&temporaryCertificate, &temporaryPrivateKey]()
        {
            std::error_code cleanupError;
            std::filesystem::remove(temporaryCertificate, cleanupError);
            std::filesystem::remove(temporaryPrivateKey, cleanupError);
        };
        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(temporaryCertificate, *encodedChain, std::nullopt, &writeError))
        {
            removeTemporary();
            return std::unexpected("写入待验证的证书材料失败：" + writeError);
        }
        if (const auto privateKeyWritten = writePrivateKeyFile(temporaryPrivateKey, m_pending.privateKeyPem); !privateKeyWritten)
        {
            removeTemporary();
            return std::unexpected("写入待验证的证书材料失败：" + privateKeyWritten.error());
        }
        const auto tlsVerified = verifyTlsIdentity(m_configuration, temporaryCertificate, temporaryPrivateKey);
        removeTemporary();
        if (!tlsVerified)
        {
            return std::unexpected(tlsVerified.error());
        }

        std::string previousCertificate;
        std::string previousPrivateKey;
        if (fileExists(certificateFile))
        {
            const auto read = readTextFile(certificateFile);
            if (!read)
            {
                return std::unexpected(read.error());
            }
            previousCertificate = *read;
        }
        if (fileExists(privateKeyFile))
        {
            const auto read = readTextFile(privateKeyFile);
            if (!read)
            {
                return std::unexpected(read.error());
            }
            previousPrivateKey = *read;
        }

        // 磁盘备份留下来供启动恢复与人工核对；内存备份供同一进程内的 reload 失败时立即回退
        const std::filesystem::path certificateBackup = certificateFile.string() + ".bak";
        const std::filesystem::path privateKeyBackup  = privateKeyFile.string() + ".bak";
        if (!previousCertificate.empty() && !AsynGyanis::Platform::AtomicFileWriter::writeText(certificateBackup, previousCertificate, std::nullopt, &writeError))
        {
            return std::unexpected("备份现有证书失败：" + writeError);
        }
        if (!previousPrivateKey.empty())
        {
            if (const auto backupWritten = writePrivateKeyFile(privateKeyBackup, previousPrivateKey); !backupWritten)
            {
                return std::unexpected("备份现有私钥失败：" + backupWritten.error());
            }
        }

        // 标记写在两份正式文件被替换之前：进程若死在两次替换之间，下次启动按它找回上一对材料
        const std::filesystem::path markerPath = installMarkerPath(m_configuration);
        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(markerPath, installMarkerText(m_configuration), std::nullopt, &writeError))
        {
            return std::unexpected("写入证书安装标记失败：" + writeError);
        }

        m_previousCertificatePem = previousCertificate;
        m_previousPrivateKeyPem  = previousPrivateKey;
        m_hasPrevious            = true;
        const auto rollback      = [this, &markerPath]() -> std::expected<void, std::string>
        {
            const auto restored = restoreFromMemory();
            if (!restored)
            {
                return restored;
            }
            return removeFileChecked(markerPath, "证书安装标记");
        };

        // 先写私钥再写证书：证书是服务端认可的身份凭据，私钥没落好之前先换上证书只会留下半对材料
        if (const auto keyWritten = writePrivateKeyFile(privateKeyFile, m_pending.privateKeyPem); !keyWritten)
        {
            const auto restored = rollback();
            if (!restored)
            {
                LOG_ERROR_FMT("安装新私钥失败后回退旧材料也失败：{}", restored.error());
            }
            return std::unexpected("写入新私钥失败：" + keyWritten.error());
        }
        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(certificateFile, *encodedChain, std::nullopt, &writeError))
        {
            const auto restored = rollback();
            if (!restored)
            {
                LOG_ERROR_FMT("安装新证书失败后回退旧材料也失败：{}", restored.error());
            }
            return std::unexpected("写入新证书失败：" + writeError);
        }

        const auto identity = AgentIdentity::load(m_configuration);
        if (!identity)
        {
            const auto restored = rollback();
            if (!restored)
            {
                LOG_ERROR_FMT("新身份加载失败后回退旧材料也失败：{}", restored.error());
            }
            return std::unexpected("安装后的身份加载失败：" + identity.error());
        }

        // 两份正式文件都已换好，标记不再需要；删不掉时下次启动会复核配对后自行清理
        if (const auto cleared = removeFileChecked(markerPath, "证书安装标记"); !cleared)
        {
            LOG_WARN_FMT("{}；下次启动会复核证书配对后清理", cleared.error());
        }
        m_pending = CertificateRequestMaterial{};
        return *identity;
    }

    void AgentCertificateManager::commitInstalledIdentity()
    {
        const auto certificateBackup = std::filesystem::path{m_configuration.certificateFile.string() + ".bak"};
        const auto privateKeyBackup  = std::filesystem::path{m_configuration.privateKeyFile.string() + ".bak"};
        if (const auto removed = removeFileChecked(certificateBackup, "证书备份"); !removed)
        {
            LOG_WARN_FMT("{}", removed.error());
        }
        if (const auto removed = removeFileChecked(privateKeyBackup, "私钥备份"); !removed)
        {
            LOG_WARN_FMT("{}", removed.error());
        }
        m_hasPrevious = false;
        m_previousCertificatePem.clear();
        m_previousPrivateKeyPem.clear();
    }

    std::expected<bool, std::string> AgentCertificateManager::recoverInterruptedInstall(const AgentConfiguration &configuration)
    {
        const std::filesystem::path markerPath = installMarkerPath(configuration);
        std::error_code             existsError;
        if (!std::filesystem::exists(markerPath, existsError))
        {
            if (existsError)
            {
                return std::unexpected("检查证书安装标记失败：" + AsynGyanis::Platform::FileSystem::utf8FromPath(markerPath) + "：" + existsError.message());
            }
            return false;
        }

        const auto marker = readInstallMarker(markerPath);
        if (!marker)
        {
            return std::unexpected(marker.error());
        }
        if (marker->certificate != configuration.certificateFile || marker->privateKey != configuration.privateKeyFile)
        {
            return std::unexpected("证书安装标记记录的路径与当前配置不一致，请人工核对后删除标记：" + AsynGyanis::Platform::FileSystem::utf8FromPath(markerPath));
        }

        const auto pairLoaded = verifyTlsIdentity(configuration, configuration.certificateFile, configuration.privateKeyFile);
        if (pairLoaded)
        {
            const auto cleared = removeFileChecked(markerPath, "证书安装标记");
            if (!cleared)
            {
                return std::unexpected(cleared.error());
            }
            LOG_INFO("检测到上次证书安装的标记，配置路径上的证书与私钥可以正常装载，已清理标记");
            return false;
        }

        LOG_WARN_FMT("上次证书安装在中途中断（{}），开始用安装前的备份恢复", pairLoaded.error());
        const auto certificateBackupText = readTextFile(marker->certificateBackup);
        if (!certificateBackupText)
        {
            return std::unexpected("恢复中断的证书安装失败：" + certificateBackupText.error());
        }
        const auto privateKeyBackupText = readTextFile(marker->privateKeyBackup);
        if (!privateKeyBackupText)
        {
            return std::unexpected("恢复中断的证书安装失败：" + privateKeyBackupText.error());
        }

        std::string writeError;
        if (const auto privateKeyWritten = writePrivateKeyFile(configuration.privateKeyFile, *privateKeyBackupText); !privateKeyWritten)
        {
            return std::unexpected("恢复上一份私钥失败：" + privateKeyWritten.error());
        }
        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(configuration.certificateFile, *certificateBackupText, std::nullopt, &writeError))
        {
            return std::unexpected("恢复上一份证书失败：" + writeError);
        }

        const auto restored = verifyTlsIdentity(configuration, configuration.certificateFile, configuration.privateKeyFile);
        if (!restored)
        {
            return std::unexpected("用备份恢复后证书与私钥仍无法装载，保留安装标记等待人工处理：" + restored.error());
        }
        const auto cleared = removeFileChecked(markerPath, "证书安装标记");
        if (!cleared)
        {
            return std::unexpected(cleared.error());
        }
        if (const auto removed = removeFileChecked(marker->certificateBackup, "证书备份"); !removed)
        {
            LOG_WARN_FMT("{}", removed.error());
        }
        if (const auto removed = removeFileChecked(marker->privateKeyBackup, "私钥备份"); !removed)
        {
            LOG_WARN_FMT("{}", removed.error());
        }
        LOG_WARN("已用安装前的备份恢复证书与私钥，并清理安装标记");
        return true;
    }

    std::expected<void, std::string> AgentCertificateManager::restoreFromMemory()
    {
        std::string writeError;
        if (m_previousPrivateKeyPem.empty())
        {
            if (const auto removed = removeFileChecked(m_configuration.privateKeyFile, "新私钥"); !removed)
            {
                return removed;
            }
        } else if (const auto privateKeyWritten = writePrivateKeyFile(m_configuration.privateKeyFile, m_previousPrivateKeyPem); !privateKeyWritten)
        {
            return std::unexpected("恢复上一份私钥失败：" + privateKeyWritten.error());
        }
        if (m_previousCertificatePem.empty())
        {
            if (const auto removed = removeFileChecked(m_configuration.certificateFile, "新证书"); !removed)
            {
                return removed;
            }
        } else if (!AsynGyanis::Platform::AtomicFileWriter::writeText(m_configuration.certificateFile, m_previousCertificatePem, std::nullopt, &writeError))
        {
            return std::unexpected("恢复上一份证书失败：" + writeError);
        }
        return {};
    }
} // namespace EdgePulse::Agent
