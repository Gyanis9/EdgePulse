#include "AgentCertificateManager.h"

#include "Platform/FileSystem/AtomicFileWriter.h"

#include "Core/Tls/TlsContext.h"
#include "Core/Tls/TlsPolicy.h"

#include "Base/Log/LogMacros.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
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
            if (certificates.empty())
            {
                return std::unexpected("服务端返回的证书不是 PEM 编码的 X.509 证书");
            }
            return certificates;
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
    } // namespace

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
        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(temporaryCertificate, std::string{certificatePem}, std::nullopt, &writeError) ||
            !AsynGyanis::Platform::AtomicFileWriter::writeText(temporaryPrivateKey, m_pending.privateKeyPem, kPrivateKeyPermissions, &writeError))
        {
            removeTemporary();
            return std::unexpected("写入待验证的证书材料失败：" + writeError);
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

        // 磁盘备份留下来供人工恢复；内存备份供同一进程内的 reload 失败时立即回退
        const std::filesystem::path certificateBackup = certificateFile.string() + ".bak";
        const std::filesystem::path privateKeyBackup  = privateKeyFile.string() + ".bak";
        if (!previousCertificate.empty() && !AsynGyanis::Platform::AtomicFileWriter::writeText(certificateBackup, previousCertificate, std::nullopt, &writeError))
        {
            return std::unexpected("备份现有证书失败：" + writeError);
        }
        if (!previousPrivateKey.empty() && !AsynGyanis::Platform::AtomicFileWriter::writeText(privateKeyBackup, previousPrivateKey, kPrivateKeyPermissions, &writeError))
        {
            return std::unexpected("备份现有私钥失败：" + writeError);
        }

        m_previousCertificatePem = previousCertificate;
        m_previousPrivateKeyPem  = previousPrivateKey;
        m_hasPrevious            = true;
        const auto rollback      = [this]() -> std::expected<void, std::string> { return restoreFromMemory(); };

        // 先写私钥再写证书：证书是服务端认可的身份凭据，私钥没落好之前先换上证书只会留下半对材料
        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(privateKeyFile, m_pending.privateKeyPem, kPrivateKeyPermissions, &writeError))
        {
            const auto restored = rollback();
            if (!restored)
            {
                LOG_ERROR_FMT("安装新私钥失败后回退旧材料也失败：{}", restored.error());
            }
            return std::unexpected("写入新私钥失败：" + writeError);
        }
        if (!AsynGyanis::Platform::AtomicFileWriter::writeText(certificateFile, std::string{certificatePem}, std::nullopt, &writeError))
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

        m_pending = CertificateRequestMaterial{};
        return *identity;
    }

    std::expected<void, std::string> AgentCertificateManager::restorePrevious()
    {
        if (!m_hasPrevious)
        {
            return std::unexpected("没有可恢复的上一份证书");
        }
        const auto restored = restoreFromMemory();
        if (!restored)
        {
            return restored;
        }
        m_hasPrevious = false;
        m_previousCertificatePem.clear();
        m_previousPrivateKeyPem.clear();
        return {};
    }

    std::expected<void, std::string> AgentCertificateManager::restoreFromMemory()
    {
        std::string writeError;
        if (m_previousPrivateKeyPem.empty())
        {
            std::error_code removeError;
            std::filesystem::remove(m_configuration.privateKeyFile, removeError);
        } else if (!AsynGyanis::Platform::AtomicFileWriter::writeText(m_configuration.privateKeyFile, m_previousPrivateKeyPem, kPrivateKeyPermissions, &writeError))
        {
            return std::unexpected("恢复上一份私钥失败：" + writeError);
        }
        if (m_previousCertificatePem.empty())
        {
            std::error_code removeError;
            std::filesystem::remove(m_configuration.certificateFile, removeError);
        } else if (!AsynGyanis::Platform::AtomicFileWriter::writeText(m_configuration.certificateFile, m_previousCertificatePem, std::nullopt, &writeError))
        {
            return std::unexpected("恢复上一份证书失败：" + writeError);
        }
        return {};
    }

    void AgentCertificateManager::discardPending()
    {
        m_pending = CertificateRequestMaterial{};
    }
} // namespace EdgePulse::Agent
